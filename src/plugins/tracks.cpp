#include "core/cot_detail.h"
#include "core/delete_confirmation.h"
#include "core/icon_cache.h"
#include "core/interfaces.h"
#include "core/logging.h"
#include "core/map_label.h"
#include "core/map_tools.h"
#include "core/marker_symbols.h"
#include "core/preferences.h"
#include "core/projection.h"
#include "core/store.h"
#include "core/tak_drawings.h"
#include "core/ui_icons.h"

#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStyle>
#include <QTableView>

#include <algorithm>

namespace swak {
namespace {
QPointF screenPoint(const Viewport &view, QPointF world)
{
    return (world - view.center) * view.scale() +
           QPointF(view.size.width() / 2, view.size.height() / 2);
}
QPainterPath headingCone(QPointF point, double heading, double radius, double angle)
{
    QPainterPath path;
    path.moveTo(point);
    path.arcTo(QRectF(point.x() - radius, point.y() - radius, radius * 2, radius * 2),
               90 - heading + angle / 2,
               -angle);
    path.closeSubpath();
    return path;
}
class HeadingConeTool final : public MapTool, public SettingsParticipant
{
public:
    HeadingConeTool(TrackMotion &motion, QObject *parent) : MapTool(parent), motion_(motion)
    {
        setObjectName(QStringLiteral("fovSettings"));
        connect(&motion_, &TrackMotion::coneTargetsChanged, this, &MapTool::actionsChanged);
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.fov");
    }
    // Earlier profiles kept the cone style in the CoT animation settings.
    QString preferenceFallbackId() const override
    {
        return QStringLiteral("org.swak.track-motion");
    }
    QString settingsTitle() const override
    {
        return tr("FOV");
    }
    QList<SettingField> settingsFields() const override
    {
        return {
            booleanSetting(QStringLiteral("headingConeEnabled"), tr("Show FOV cones")),
            numberSetting(QStringLiteral("headingConeAngle"), tr("Cone angle (degrees)"), 5, 180),
            numberSetting(QStringLiteral("headingConeLength"), tr("Cone length (pixels)"), 10, 200),
            numberSetting(QStringLiteral("headingConeOpacity"), tr("Fill opacity"), .01, 1, 2),
            numberSetting(QStringLiteral("headingConeOutline"),
                          tr("Outline width (pixels, 0 = none)"),
                          0,
                          5,
                          1),
            numberSetting(
                QStringLiteral("headingConeOutlineOpacity"), tr("Outline opacity"), .05, 1, 2)};
    }
    QJsonObject savePreferences() const override
    {
        const auto &cone = motion_.headingConeStyle();
        return {{QStringLiteral("headingConeEnabled"), cone.enabled},
                {QStringLiteral("headingConeAngle"), cone.angle},
                {QStringLiteral("headingConeLength"), cone.length},
                {QStringLiteral("headingConeOpacity"), cone.opacity},
                {QStringLiteral("headingConeOutline"), cone.outlineWidth},
                {QStringLiteral("headingConeOutlineOpacity"), cone.outlineOpacity}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        HeadingConeStyle cone;
        cone.enabled = values.value(QStringLiteral("headingConeEnabled")).toBool(true);
        cone.angle = preferenceNumber(values, QStringLiteral("headingConeAngle"), 60, 5, 180);
        cone.length = preferenceNumber(values, QStringLiteral("headingConeLength"), 60, 10, 200);
        cone.opacity = preferenceNumber(values, QStringLiteral("headingConeOpacity"), .15, .01, 1);
        cone.outlineWidth = preferenceNumber(values, QStringLiteral("headingConeOutline"), 0, 0, 5);
        cone.outlineOpacity =
            preferenceNumber(values, QStringLiteral("headingConeOutlineOpacity"), .6, .05, 1);
        motion_.setHeadingConeStyle(cone);
    }
    // A click inside a visible cone selects the object that owns the cone.
    QList<MapItem> itemsAt(const Viewport &view, QPointF position) const override
    {
        QList<MapItem> result;
        const auto &cone = motion_.headingConeStyle();
        if (!cone.enabled) {
            return result;
        }
        const auto now = QDateTime::currentDateTimeUtc();
        for (const auto &visual : motion_.snapshot()) {
            const auto &event = visual.event;
            if (!visual.kinematics.heading || !motion_.headingConeEnabled(event.uid) ||
                visual.expired || visual.opacity < 0.15 || event.start > now ||
                event.stale <= now) {
                continue;
            }
            if (headingCone(screenPoint(view, visual.world),
                            *visual.kinematics.heading,
                            view.markerSize(cone.length),
                            cone.angle)
                    .contains(position)) {
                MapItem item;
                item.owner = QStringLiteral("org.swak.events");
                item.id = event.uid;
                item.label = (event.callsign.isEmpty() ? event.uid : event.callsign).left(32);
                result.append(std::move(item));
            }
        }
        return result;
    }
    void populateInformationMenu(const MapItem &item, QMenu &menu) override
    {
        populateContextMenu(item, menu);
    }
    void populateContextMenu(const MapItem &item, QMenu &menu) override
    {
        if (item.owner != QStringLiteral("org.swak.events")) {
            return;
        }
        auto *action = menu.addAction(tr("FOV cone"));
        action->setObjectName(QStringLiteral("headingConeObject"));
        action->setCheckable(true);
        action->setChecked(motion_.headingConeEnabled(item.id));
        action->setEnabled(motion_.canEnableHeadingCone(item.id));
        action->setToolTip(
            action->isEnabled()
                ? tr("Show the field of view cone for this object.")
                : tr("The cone limit is 4096 objects. Turn off another cone first."));
        connect(action, &QAction::triggered, this, [this, id = item.id](bool enabled) {
            motion_.setHeadingConeEnabled(id, enabled);
        });
    }

private:
    TrackMotion &motion_;
};
class EventTable final : public QTableView, public SettingsParticipant
{
public:
    explicit EventTable(QWidget *parent) : QTableView(parent)
    {
        connect(horizontalHeader(),
                &QHeaderView::sectionResized,
                this,
                [this](int column, int, int width) {
                    if (width > 0) {
                        widths_.insert(column, width);
                    }
                });
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.events");
    }
    QString settingsTitle() const override
    {
        return tr("CoT Messages");
    }
    QList<SettingField> settingsFields() const override
    {
        QList<SettingField> fields{
            choiceSetting(
                QStringLiteral("sortColumn"),
                tr("Sort column"),
                {tr("Identity"), tr("Type"), tr("Latitude"), tr("Longitude"), tr("Stale time")},
                QJsonArray::fromStringList(columnIds())),
            choiceSetting(QStringLiteral("sortOrder"),
                          tr("Sort order"),
                          {tr("Ascending"), tr("Descending")})};
        const auto columns = savePreferences().value(QStringLiteral("columns")).toArray();
        const QStringList names{
            tr("Identity"), tr("Type"), tr("Latitude"), tr("Longitude"), tr("Stale time")};
        for (int i = 0; i < columns.size(); ++i) {
            const auto column = columns.at(i).toObject();
            const auto title =
                names.at(columnIds().indexOf(column.value(QStringLiteral("id")).toString()));
            const auto path = QStringLiteral("columns/%1/").arg(i);
            fields.append(
                numberSetting(path + QStringLiteral("width"), tr("%1 width").arg(title), 24, 4096));
            fields.append(
                booleanSetting(path + QStringLiteral("hidden"), tr("Hide %1").arg(title)));
            fields.append(numberSetting(
                path + QStringLiteral("position"), tr("%1 position (0–4)").arg(title), 0, 4));
        }
        return fields;
    }
    QString validateSettings(const QJsonObject &values) const override
    {
        QSet<int> positions;
        for (const auto &entry : values.value(QStringLiteral("columns")).toArray()) {
            const int position = entry.toObject().value(QStringLiteral("position")).toInt(-1);
            if (position < 0 || position >= columnIds().size() || positions.contains(position)) {
                return tr("Use each column position once, from 0 through 4.");
            }
            positions.insert(position);
        }
        return {};
    }
    QJsonObject savePreferences() const override
    {
        QJsonArray columns;
        const auto *header = horizontalHeader();
        for (int visual = 0; visual < header->count(); ++visual) {
            const int logical = header->logicalIndex(visual);
            columns.append(
                QJsonObject{{QStringLiteral("id"), columnIds().at(logical)},
                            {QStringLiteral("position"), visual},
                            {QStringLiteral("width"),
                             header->isSectionHidden(logical) ? widths_.value(logical, 100)
                                                              : header->sectionSize(logical)},
                            {QStringLiteral("hidden"), header->isSectionHidden(logical)}});
        }
        const int sort = header->sortIndicatorSection();
        return {{QStringLiteral("columns"), columns},
                {QStringLiteral("sortColumn"),
                 sort >= 0 && sort < columnIds().size() ? columnIds().at(sort) : QString{}},
                {QStringLiteral("sortOrder"), static_cast<int>(header->sortIndicatorOrder())}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        QList<QJsonObject> columns;
        for (const auto &entry : values.value(QStringLiteral("columns")).toArray()) {
            auto column = entry.toObject();
            if (!column.contains(QStringLiteral("position"))) {
                column.insert(QStringLiteral("position"), static_cast<int>(columns.size()));
            }
            columns.append(column);
        }
        std::stable_sort(
            columns.begin(), columns.end(), [](const QJsonObject &a, const QJsonObject &b) {
                return a.value(QStringLiteral("position")).toInt() <
                       b.value(QStringLiteral("position")).toInt();
            });
        auto *header = horizontalHeader();
        QSet<int> restored;
        for (const auto &entry : columns) {
            const auto column = entry;
            const int logical = static_cast<int>(
                columnIds().indexOf(column.value(QStringLiteral("id")).toString()));
            if (logical < 0 || restored.contains(logical)) {
                continue;
            }
            const int visual = static_cast<int>(restored.size());
            restored.insert(logical);
            header->moveSection(header->visualIndex(logical), visual);
            header->resizeSection(
                logical, preferenceInteger(column, QStringLiteral("width"), 100, 24, 4096));
            header->setSectionHidden(logical, column.value(QStringLiteral("hidden")).toBool(false));
        }
        const int sort = static_cast<int>(
            columnIds().indexOf(values.value(QStringLiteral("sortColumn")).toString()));
        if (sort >= 0) {
            sortByColumn(sort,
                         static_cast<Qt::SortOrder>(preferenceInteger(
                             values, QStringLiteral("sortOrder"), Qt::AscendingOrder, 0, 1)));
        }
    }

private:
    QHash<int, int> widths_;
    static const QStringList &columnIds()
    {
        static const QStringList ids{QStringLiteral("identity"),
                                     QStringLiteral("type"),
                                     QStringLiteral("latitude"),
                                     QStringLiteral("longitude"),
                                     QStringLiteral("stale")};
        return ids;
    }
};
} // namespace
class TracksPlugin final : public QObject,
                           public Plugin,
                           public MapLayer,
                           public ToolPlugin,
                           public MenuPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "tracks/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapLayer swak::ToolPlugin swak::MenuPlugin)
public:
    QString name() const override
    {
        return tr("CoT Messages");
    }
    void populateMenu(PluginServices &services, QMenu &menu) override
    {
        menu.addAction(createClearAction(services.store, menu));
        const auto *key = &services.store;
        connect(&services.store, &QObject::destroyed, this, [this, key] {
            detailStyles_.remove(key);
            iconCaches_.remove(key);
        });
        if (services.mapTools) {
            auto *cone = new HeadingConeTool(services.store.motion(), &menu);
            services.mapTools->add(cone);
            if (services.icons) {
                iconCaches_.insert(key, services.icons);
                // A loaded icon needs a new frame. Tool changes repaint the map.
                connect(services.icons, &IconCache::iconLoaded, cone, &MapTool::changed);
            }
        }
    }
    int order() const override
    {
        return 100;
    }
    void paint(QPainter &painter, const Viewport &view, const Store &store) const override
    {
        const auto now = QDateTime::currentDateTimeUtc();
        const QRectF visible(QPointF(0, 0), view.size);
        int labels = 0;
        for (const auto &visual : store.motion().snapshot()) {
            const auto &event = visual.event;
            // The TAK Drawings layer draws shapes and routes with their labels.
            if (event.start > now || takDrawingType(event.type)) {
                continue;
            }
            const QPointF point = screenPoint(view, visual.world);
            if (!visible
                     .adjusted(-view.markerSize(12),
                               -view.markerSize(12),
                               view.markerSize(12),
                               view.markerSize(12))
                     .contains(point)) {
                continue;
            }
            const bool selected = event.uid == store.selected();
            const auto symbol = markerSymbolForCot(event.type);
            QColor color = event.type.startsWith(QLatin1String("a-f-"))   ? QColor("#54c8ff")
                           : event.type.startsWith(QLatin1String("a-h-")) ? QColor("#ff7a7a")
                                                                          : QColor("#f6d477");
            const auto &symbols = markerSymbols();
            const auto found =
                std::find_if(symbols.cbegin(), symbols.cend(), [&symbol](const auto &entry) {
                    return entry.code == symbol;
                });
            if (found != symbols.cend()) {
                color = found->color;
            }
            const auto &style = detailStyle(store, event);
            if (event.type == u"b-m-p-s-m" && style.color.isValid()) {
                color = style.color;
            }
            QImage icon;
            if (!style.iconPath.isEmpty()) {
                if (const auto cache = iconCaches_.value(&store)) {
                    icon = cache->icon(event.origin, style.iconPath);
                }
            }
            const auto gray = qGray(color.rgb());
            color.setRed(qRound(color.red() * (1 - visual.gray) + gray * visual.gray));
            color.setGreen(qRound(color.green() * (1 - visual.gray) + gray * visual.gray));
            color.setBlue(qRound(color.blue() * (1 - visual.gray) + gray * visual.gray));
            painter.save();
            painter.setOpacity(visual.opacity);
            const auto &cone = store.motion().headingConeStyle();
            if (cone.enabled && store.motion().headingConeEnabled(event.uid) &&
                visual.kinematics.heading) {
                const auto path = headingCone(
                    point, *visual.kinematics.heading, view.markerSize(cone.length), cone.angle);
                painter.setOpacity(visual.opacity * cone.opacity);
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawPath(path);
                if (cone.outlineWidth > 0) {
                    painter.setOpacity(visual.opacity * cone.outlineOpacity);
                    painter.setPen(
                        QPen(color, cone.outlineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawPath(path);
                }
                painter.setOpacity(visual.opacity);
            }
            if (icon.isNull()) {
                paintMarkerSymbol(painter,
                                  symbol,
                                  point,
                                  view.markerSize(6) + (selected ? 2 : 0),
                                  color,
                                  selected);
            } else {
                // A server icon replaces the symbol. A missing icon keeps the symbol.
                const double side = view.markerSize(12) + (selected ? 4 : 0);
                QSizeF size(icon.size());
                size.scale(side, side, Qt::KeepAspectRatio);
                painter.drawImage(
                    QRectF(point - QPointF(size.width() / 2, size.height() / 2), size), icon);
                if (selected) {
                    painter.setPen(QPen(Qt::white, 2));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawEllipse(point, side * 0.7, side * 0.7);
                }
            }
            if (selected || labels++ < 100) {
                paintMapLabel(painter,
                              point + view.markerOffset(QPointF(16, -8), 6),
                              (event.callsign.isEmpty() ? event.uid : event.callsign).left(32),
                              selected);
            }
            painter.restore();
        }
    }
    QWidget *create(PluginServices &services, const Codec &, QWidget *parent) override
    {
        auto &store = services.store;
        auto *table = new EventTable(parent);
        table->setObjectName(QStringLiteral("eventsTable"));
        table->setAccessibleName(tr("Received and generated events"));
        auto *proxy = new QSortFilterProxyModel(table);
        proxy->setSourceModel(&store);
        proxy->setSortRole(Store::SortRole);
        proxy->setSortLocaleAware(true);
        table->setModel(proxy);
        table->setSortingEnabled(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->horizontalHeader()->setSectionsMovable(true);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setStretchLastSection(true);
        connect(table->selectionModel(),
                &QItemSelectionModel::currentRowChanged,
                table,
                [&store](const QModelIndex &current) {
                    if (current.isValid()) {
                        store.select(current.data(Qt::UserRole).toString());
                    }
                });
        connect(&store, &Store::selectionChanged, table, [table, proxy, &store] {
            for (int row = 0; row < proxy->rowCount(); ++row) {
                if (proxy->index(row, 0).data(Qt::UserRole).toString() == store.selected()) {
                    table->selectRow(row);
                    return;
                }
            }
            table->selectionModel()->clear();
        });
        return table;
    }

private:
    struct DetailStyle
    {
        QDateTime time;
        qsizetype detailSize = 0;
        // The color element, which TAK spot markers use.
        QColor color;
        // The usericon iconset path of a server icon, or empty.
        QString iconPath;
    };
    // Returns the style elements of an event. It reads the detail only when the event
    // changes, so frames do not parse detail XML.
    const DetailStyle &detailStyle(const Store &store, const Event &event) const
    {
        auto &cache = detailStyles_[&store];
        auto it = cache.find(event.uid);
        if (it == cache.end() || it->time != event.time || it->detailSize != event.detail.size()) {
            if (cache.size() >= MaxEvents) {
                cache.clear();
            }
            auto color = detailColor(event, u"color").value_or(QColor());
            if (color.isValid()) {
                color.setAlpha(255);
            }
            auto path = iconsetPath(event);
            if (!serverIconPath(path) || event.origin.isEmpty()) {
                path.clear();
            }
            it = cache.insert(event.uid, {event.time, event.detail.size(), color, path});
        }
        return *it;
    }
    QAction *createClearAction(Store &store, QWidget &owner)
    {
        auto *clear = new QAction(uiIcon(UiIcon::Clear), tr("Clear ..."), &owner);
        clear->setObjectName(QStringLiteral("clearEventsAction"));
        clear->setToolTip(tr("Clear CoT Messages"));
        clear->setStatusTip(tr("Remove all current CoT messages and clear the selection."));
        connect(clear, &QAction::triggered, &store, [&store, &owner] {
            if (!confirmAction(
                    &owner, tr("Clear CoT messages"), tr("Clear all CoT messages?"), tr("Clear"))) {
                return;
            }
            store.clear();
            logMessage(Severity::Info,
                       Urgency::Routine,
                       QStringLiteral("swak.events"),
                       tr("CoT Messages cleared."));
        });
        return clear;
    }
    // The cache changes inside const paint calls. Only the interface thread uses it.
    mutable QHash<const Store *, QHash<QString, DetailStyle>> detailStyles_;
    QHash<const Store *, QPointer<IconCache>> iconCaches_;
};
} // namespace swak
#include "tracks.moc"

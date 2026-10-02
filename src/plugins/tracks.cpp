#include "core/interfaces.h"
#include "core/projection.h"
#include "core/store.h"
#include "core/preferences.h"
#include <QJsonArray>
#include <QHeaderView>
#include <QPainter>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <algorithm>

namespace swak {
class EventTable final : public QTableView, public SettingsParticipant {
public:
    explicit EventTable(QWidget *parent) : QTableView(parent)
    {
        connect(horizontalHeader(), &QHeaderView::sectionResized, this, [this](int column, int, int width) {
            if (width > 0) widths_.insert(column, width);
        });
    }
    QString preferenceId() const override { return QStringLiteral("org.swak.events"); }
    QString settingsTitle() const override { return tr("CoT Events"); }
    QList<SettingField> settingsFields() const override
    {
        QList<SettingField> fields{choiceSetting(QStringLiteral("sortColumn"), tr("Sort column"),
            {tr("Identity"), tr("Type"), tr("Latitude"), tr("Longitude"), tr("Stale time")}, QJsonArray::fromStringList(columnIds())),
            choiceSetting(QStringLiteral("sortOrder"), tr("Sort order"), {tr("Ascending"), tr("Descending")})};
        const auto columns = savePreferences().value(QStringLiteral("columns")).toArray();
        const QStringList names{tr("Identity"), tr("Type"), tr("Latitude"), tr("Longitude"), tr("Stale time")};
        for (int i = 0; i < columns.size(); ++i) {
            const auto column = columns.at(i).toObject();
            const auto title = names.at(columnIds().indexOf(column.value(QStringLiteral("id")).toString()));
            const auto path = QStringLiteral("columns/%1/").arg(i);
            fields.append(numberSetting(path + QStringLiteral("width"), tr("%1 width").arg(title), 24, 4096));
            fields.append(booleanSetting(path + QStringLiteral("hidden"), tr("Hide %1").arg(title)));
            fields.append(numberSetting(path + QStringLiteral("position"), tr("%1 position (0–4)").arg(title), 0, 4));
        }
        return fields;
    }
    QString validateSettings(const QJsonObject &values) const override
    {
        QSet<int> positions;
        for (const auto &entry : values.value(QStringLiteral("columns")).toArray()) {
            const int position = entry.toObject().value(QStringLiteral("position")).toInt(-1);
            if (position < 0 || position >= columnIds().size() || positions.contains(position))
                return tr("Use each column position once, from 0 through 4.");
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
            columns.append(QJsonObject{{QStringLiteral("id"), columnIds().at(logical)},
                {QStringLiteral("position"), visual},
                {QStringLiteral("width"), header->isSectionHidden(logical) ? widths_.value(logical, 100) : header->sectionSize(logical)},
                {QStringLiteral("hidden"), header->isSectionHidden(logical)}});
        }
        const int sort = header->sortIndicatorSection();
        return {{QStringLiteral("columns"), columns},
                {QStringLiteral("sortColumn"), sort >= 0 && sort < columnIds().size() ? columnIds().at(sort) : QString{}},
                {QStringLiteral("sortOrder"), static_cast<int>(header->sortIndicatorOrder())}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        QList<QJsonObject> columns;
        for (const auto &entry : values.value(QStringLiteral("columns")).toArray()) {
            auto column = entry.toObject();
            if (!column.contains(QStringLiteral("position"))) column.insert(QStringLiteral("position"), static_cast<int>(columns.size()));
            columns.append(column);
        }
        std::stable_sort(columns.begin(), columns.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value(QStringLiteral("position")).toInt() < b.value(QStringLiteral("position")).toInt();
        });
        auto *header = horizontalHeader();
        QSet<int> restored;
        for (const auto &entry : columns) {
            const auto column = entry;
            const int logical = static_cast<int>(columnIds().indexOf(column.value(QStringLiteral("id")).toString()));
            if (logical < 0 || restored.contains(logical)) continue;
            const int visual = static_cast<int>(restored.size());
            restored.insert(logical);
            header->moveSection(header->visualIndex(logical), visual);
            header->resizeSection(logical, preferenceInteger(column, QStringLiteral("width"), 100, 24, 4096));
            header->setSectionHidden(logical, column.value(QStringLiteral("hidden")).toBool(false));
        }
        const int sort = static_cast<int>(columnIds().indexOf(values.value(QStringLiteral("sortColumn")).toString()));
        if (sort >= 0) sortByColumn(sort, static_cast<Qt::SortOrder>(
            preferenceInteger(values, QStringLiteral("sortOrder"), Qt::AscendingOrder, 0, 1)));
    }
private:
    QHash<int, int> widths_;
    static const QStringList &columnIds()
    {
        static const QStringList ids{QStringLiteral("identity"), QStringLiteral("type"), QStringLiteral("latitude"),
                                     QStringLiteral("longitude"), QStringLiteral("stale")};
        return ids;
    }
};
class TracksPlugin final : public QObject, public MapLayer, public ToolPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.MapLayer/1.0")
    Q_INTERFACES(swak::MapLayer swak::ToolPlugin)
public:
    QString name() const override { return tr("CoT Events"); }
    int order() const override { return 100; }
    void paint(QPainter &painter, const Viewport &view, const Store &store) const override
    {
        const auto now = QDateTime::currentDateTimeUtc();
        const QRectF visible(QPointF(0, 0), view.size);
        int labels = 0;
        for (const auto &event : store.events()) {
            if (event.start > now || event.stale <= now) continue;
            const QPointF point = view.screen(event.latitude, event.longitude);
            if (!visible.adjusted(-12, -12, 12, 12).contains(point)) continue;
            const bool selected = event.uid == store.selected();
            painter.setPen(QPen(selected ? QColor("#ffffff") : QColor("#13202a"), selected ? 3 : 2));
            painter.setBrush(event.type.startsWith(QLatin1String("a-f")) ? QColor("#54c8ff") :
                             event.type.startsWith(QLatin1String("a-h")) ? QColor("#ff7a7a") : QColor("#f6d477"));
            painter.drawEllipse(point, selected ? 8 : 6, selected ? 8 : 6);
            if (selected || labels++ < 100) {
                painter.setPen(QColor("#ffffff"));
                painter.drawText(point + QPointF(12, -8), (event.callsign.isEmpty() ? event.uid : event.callsign).left(32));
            }
        }
    }
    QWidget *create(Store &store, const Codec &, QWidget *parent) override
    {
        auto *table = new EventTable(parent);
        table->setObjectName(QStringLiteral("eventsTable"));
        table->setAccessibleName(tr("Received and generated events"));
        auto *proxy = new QSortFilterProxyModel(table);
        proxy->setSourceModel(&store);
        table->setModel(proxy);
        table->setSortingEnabled(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->horizontalHeader()->setSectionsMovable(true);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setStretchLastSection(true);
        connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, table,
                [&store](const QModelIndex &current) { if (current.isValid()) store.select(current.data(Qt::UserRole).toString()); });
        connect(&store, &Store::selectionChanged, table, [table, proxy, &store] {
            for (int row = 0; row < proxy->rowCount(); ++row) {
                if (proxy->index(row, 0).data(Qt::UserRole).toString() == store.selected()) {
                    table->selectRow(row);
                    return;
                }
            }
            table->clearSelection();
        });
        return table;
    }
};
}
#include "tracks.moc"

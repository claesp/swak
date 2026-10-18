#include "core/interfaces.h"
#include "core/map_label.h"
#include "core/map_tools.h"
#include "core/preferences.h"
#include "core/projection.h"
#include "core/settings.h"
#include "core/store.h"
#include "core/tak_drawings.h"

#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QTransform>

#include <algorithm>

namespace swak {
namespace {
// Pixels between a click and an outline that still select the drawing.
constexpr double HitDistance = 6;
constexpr int MaxLabels = 100;

QTransform screenTransform(const Viewport &view)
{
    QTransform transform;
    transform.translate(view.size.width() / 2, view.size.height() / 2);
    transform.scale(view.scale(), view.scale());
    transform.translate(-view.center.x(), -view.center.y());
    return transform;
}
// Keeps the parsed drawings of one window. It parses an event only when the event changes, so
// a frame parses only the events that changed since the last frame.
class DrawingTool final : public MapTool, public SettingsParticipant
{
public:
    struct Entry
    {
        QDateTime time;
        qsizetype detailSize = 0;
        QString label;
        std::optional<TakDrawing> drawing;
    };
    DrawingTool(Store &store, QObject *parent) : MapTool(parent), store_(&store)
    {
        connect(&store, &Store::changed, this, [this] {
            dirty_ = true;
        });
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.takdrawings");
    }
    QString settingsTitle() const override
    {
        return tr("TAK Drawings");
    }
    QList<SettingField> settingsFields() const override
    {
        return {booleanSetting(QStringLiteral("visible"), tr("Show received drawings and routes")),
                numberSetting(QStringLiteral("opacity"), tr("Drawing opacity"), 0.1, 1.0, 2)};
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("visible"), visible_}, {QStringLiteral("opacity"), opacity_}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        visible_ = values.value(QStringLiteral("visible")).toBool(true);
        opacity_ = preferenceNumber(values, QStringLiteral("opacity"), 1.0, 0.1, 1.0);
        emit changed();
    }
    bool visible() const
    {
        return visible_;
    }
    void setVisible(bool visible)
    {
        if (visible_ != visible) {
            visible_ = visible;
            emit changed();
        }
    }
    double opacity() const
    {
        return opacity_;
    }
    // Returns the current drawings. It parses only events that changed since the last call.
    const QHash<QString, Entry> &entries() const
    {
        if (!dirty_ || !store_) {
            return entries_;
        }
        dirty_ = false;
        QHash<QString, Entry> next;
        for (const auto &event : store_->events()) {
            if (!takDrawingType(event.type)) {
                continue;
            }
            const auto previous = entries_.constFind(event.uid);
            if (previous != entries_.cend() && previous->time == event.time &&
                previous->detailSize == event.detail.size()) {
                next.insert(event.uid, *previous);
                continue;
            }
            next.insert(event.uid,
                        {event.time,
                         event.detail.size(),
                         (event.callsign.isEmpty() ? event.uid : event.callsign).left(32),
                         readTakDrawing(event)});
        }
        entries_ = std::move(next);
        return entries_;
    }
    bool handle(QEvent &, const Viewport &, QWidget &) override
    {
        return false;
    }
    void paint(QPainter &, const Viewport &) const override {}
    bool cancel() override
    {
        return false;
    }
    // A click near an outline, or inside a circle or area, selects the drawing.
    QList<MapItem> itemsAt(const Viewport &view, QPointF position) const override
    {
        QList<MapItem> result;
        if (!visible_) {
            return result;
        }
        const auto transform = screenTransform(view);
        const auto &cached = entries();
        for (auto it = cached.cbegin(); it != cached.cend(); ++it) {
            if (!it->drawing) {
                continue;
            }
            const auto &drawing = *it->drawing;
            auto screen = transform.map(drawing.points);
            const bool closed =
                drawing.kind == TakDrawing::Kind::Circle || drawing.kind == TakDrawing::Kind::Area;
            if (closed) {
                screen.append(screen.first());
            }
            if ((closed && screen.containsPoint(position, Qt::OddEvenFill)) ||
                lineDistance(screen, position) <= HitDistance) {
                MapItem item;
                item.owner = QStringLiteral("org.swak.events");
                item.id = it.key();
                item.label = it->label;
                result.append(std::move(item));
            }
        }
        return result;
    }

private:
    QPointer<Store> store_;
    // The cache changes inside const reads. It is not configuration, and only the interface
    // thread uses it.
    mutable QHash<QString, Entry> entries_;
    mutable bool dirty_ = true;
    bool visible_ = true;
    double opacity_ = 1.0;
};
} // namespace
class TakDrawingsPlugin final : public QObject, public Plugin, public MapLayer, public MenuPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "takdrawings/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapLayer swak::MenuPlugin)
public:
    QString name() const override
    {
        return tr("TAK Drawings");
    }
    int order() const override
    {
        // Below the CoT symbols, so users stay visible on top of shapes.
        return 90;
    }
    void populateMenu(PluginServices &services, QMenu &menu) override
    {
        auto *tool = new DrawingTool(services.store, &menu);
        const auto *key = &services.store;
        tools_.insert(key, tool);
        connect(tool, &QObject::destroyed, this, [this, key] {
            tools_.remove(key);
        });
        if (services.mapTools) {
            services.mapTools->add(tool);
        }
        auto *show = menu.addAction(tr("Show TAK Drawings"));
        show->setObjectName(QStringLiteral("showTakDrawings"));
        show->setCheckable(true);
        show->setToolTip(tr("Show circles, lines, areas, and routes from TAK clients."));
        show->setChecked(tool->visible());
        connect(tool, &MapTool::changed, show, [show, tool] {
            show->setChecked(tool->visible());
        });
        connect(show, &QAction::toggled, tool, [tool](bool checked) {
            tool->setVisible(checked);
        });
    }
    void paint(QPainter &painter, const Viewport &view, const Store &store) const override
    {
        const auto tool = tools_.value(&store);
        if (!tool || !tool->visible()) {
            return;
        }
        const auto transform = screenTransform(view);
        const QRectF visible(QPointF(0, 0), view.size);
        int labels = 0;
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setOpacity(tool->opacity());
        const auto &entries = tool->entries();
        for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
            if (!it->drawing) {
                continue;
            }
            const auto &drawing = *it->drawing;
            const auto screen = transform.map(drawing.points);
            const auto bounds = screen.boundingRect();
            if (!bounds.adjusted(-8, -8, 8, 8).intersects(visible)) {
                continue;
            }
            const bool selected = it.key() == store.selected();
            QPen pen(drawing.stroke,
                     drawing.strokeWidth + (selected ? 2 : 0),
                     Qt::SolidLine,
                     Qt::RoundCap,
                     Qt::RoundJoin);
            painter.setPen(pen);
            painter.setBrush(drawing.fill.isValid() ? QBrush(drawing.fill) : QBrush(Qt::NoBrush));
            switch (drawing.kind) {
            case TakDrawing::Kind::Circle:
            case TakDrawing::Kind::Area:
                painter.drawPolygon(screen);
                break;
            case TakDrawing::Kind::Route:
                painter.drawPolyline(screen);
                painter.setBrush(drawing.stroke);
                // Check points stay readable only when the route is larger than its points.
                if (std::max(bounds.width(), bounds.height()) >
                    40.0 * static_cast<double>(screen.size())) {
                    for (const auto point : screen) {
                        painter.drawEllipse(point, 3, 3);
                    }
                }
                break;
            case TakDrawing::Kind::Line:
                painter.drawPolyline(screen);
                break;
            }
            if (selected || labels++ < MaxLabels) {
                paintMapLabel(
                    painter, transform.map(drawing.anchor) + QPointF(8, -6), it->label, selected);
            }
        }
        painter.restore();
    }

private:
    QHash<const Store *, QPointer<DrawingTool>> tools_;
};
} // namespace swak
#include "takdrawings.moc"

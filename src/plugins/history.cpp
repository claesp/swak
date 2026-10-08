#include "core/interfaces.h"
#include "core/map_label.h"
#include "core/map_tools.h"
#include "core/projection.h"
#include "core/store.h"
#include "core/track_history.h"

#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QTransform>

namespace swak {
namespace {
const QColor TrackColor(QStringLiteral("#ffd54f"));
struct Window
{
    QString label;
    qint64 seconds;
};
const QList<Window> &windows()
{
    static const QList<Window> list{{QObject::tr("Last hour"), 3600},
                                    {QObject::tr("Last 6 hours"), 6 * 3600},
                                    {QObject::tr("Last 24 hours"), 24 * 3600},
                                    {QObject::tr("Last 7 days"), 7 * 24 * 3600}};
    return list;
}
// Adds Load History and Clear History to the menus of received objects.
class HistoryTool final : public MapTool
{
public:
    HistoryTool(Store &store, TrackHistory &history, QObject *parent)
        : MapTool(parent), store_(store), history_(&history)
    {
        connect(&history, &TrackHistory::changed, this, &MapTool::changed);
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
    QString statusText() const override
    {
        return status_;
    }
    void populateInformationMenu(const MapItem &item, QMenu &menu) override
    {
        populateContextMenu(item, menu);
    }
    void populateContextMenu(const MapItem &item, QMenu &menu) override
    {
        if (!history_ || item.owner != QStringLiteral("org.swak.events")) {
            return;
        }
        const auto &events = store_.events();
        const auto found =
            std::find_if(events.cbegin(), events.cend(), [&item](const Event &event) {
                return event.uid == item.id;
            });
        if (found == events.cend() || !history_->available(found->origin)) {
            return;
        }
        auto *load = menu.addMenu(tr("Load History"));
        load->setObjectName(QStringLiteral("loadHistory"));
        const auto serverId = found->origin;
        const auto uid = found->uid;
        const auto label = found->callsign.isEmpty() ? found->uid : found->callsign;
        for (const auto &window : windows()) {
            auto *action = load->addAction(window.label);
            connect(action,
                    &QAction::triggered,
                    this,
                    [this, serverId, uid, label, seconds = window.seconds] {
                        if (!history_) {
                            return;
                        }
                        const auto end = QDateTime::currentDateTimeUtc();
                        const auto error =
                            history_->load(serverId, uid, label, end.addSecs(-seconds), end);
                        status_ =
                            error.isEmpty() ? tr("Loading the history of %1…").arg(label) : error;
                        emit changed();
                    });
        }
        if (history_->track(uid)) {
            auto *clear = menu.addAction(tr("Clear History"));
            clear->setObjectName(QStringLiteral("clearHistory"));
            connect(clear, &QAction::triggered, this, [this, uid] {
                if (history_) {
                    history_->remove(uid);
                }
            });
        }
    }
    void setStatus(const QString &text)
    {
        status_ = text;
        emit changed();
    }

private:
    Store &store_;
    QPointer<TrackHistory> history_;
    QString status_;
};
} // namespace
class HistoryPlugin final : public QObject, public Plugin, public MapLayer, public MenuPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "history/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapLayer swak::MenuPlugin)
public:
    QString name() const override
    {
        return tr("Track History");
    }
    int order() const override
    {
        // Below the CoT symbols and above shapes.
        return 95;
    }
    void populateMenu(PluginServices &services, QMenu &menu) override
    {
        auto *history = services.history;
        if (!history) {
            return;
        }
        const auto *key = &services.store;
        histories_.insert(key, history);
        connect(history, &QObject::destroyed, this, [this, key] {
            histories_.remove(key);
        });
        auto *tool = new HistoryTool(services.store, *history, &menu);
        if (services.mapTools) {
            services.mapTools->add(tool);
        }
        connect(history,
                &TrackHistory::loaded,
                tool,
                [tool, history](const QString &uid, const QString &error) {
                    const auto *track = history->track(uid);
                    tool->setStatus(!error.isEmpty() ? error
                                    : track          ? tr("%1: %n positions.",
                                                 nullptr,
                                                 static_cast<int>(track->points.size()))
                                                  .arg(track->label)
                                            : QString{});
                });
        auto *clear = menu.addAction(tr("Clear History"));
        clear->setObjectName(QStringLiteral("clearAllHistory"));
        clear->setToolTip(tr("Remove all loaded tracks from the map."));
        connect(clear, &QAction::triggered, history, &TrackHistory::clear);
    }
    void paint(QPainter &painter, const Viewport &view, const Store &store) const override
    {
        const auto history = histories_.value(&store);
        if (!history || history->tracks().isEmpty()) {
            return;
        }
        QTransform transform;
        transform.translate(view.size.width() / 2, view.size.height() / 2);
        transform.scale(view.scale(), view.scale());
        transform.translate(-view.center.x(), -view.center.y());
        const QRectF visible(QPointF(0, 0), view.size);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        for (const auto &track : history->tracks()) {
            QPolygonF line;
            line.reserve(track.points.size());
            for (const auto &point : track.points) {
                line.append(transform.map(point.world));
            }
            if (!line.boundingRect().adjusted(-8, -8, 8, 8).intersects(visible)) {
                continue;
            }
            painter.setPen(QPen(TrackColor, 2, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPolyline(line);
            painter.setPen(Qt::NoPen);
            painter.setBrush(TrackColor);
            painter.drawEllipse(line.first(), 4, 4);
            paintMapLabel(painter,
                          line.first() + QPointF(8, -6),
                          tr("%1 from %2")
                              .arg(track.label,
                                   track.points.first().time.toLocalTime().toString(
                                       QStringLiteral("MM-dd HH:mm"))));
        }
        painter.restore();
    }

private:
    QHash<const Store *, QPointer<TrackHistory>> histories_;
};
} // namespace swak
#include "history.moc"

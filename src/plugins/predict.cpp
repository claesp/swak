#include "core/interfaces.h"
#include "core/map_tools.h"
#include "core/maps.h"
#include "core/prediction.h"
#include "core/store.h"

#include <QJsonArray>
#include <QMenu>
#include <QPainter>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace swak {
namespace {
constexpr int MaxPredictions = 128;
constexpr std::array<int, 3> Minutes{1, 10, 30};
QString endpointId(const QString &uid, int minutes)
{
    return uid + '/' + QString::number(minutes);
}
class PredictTool final : public MapTool, public SettingsParticipant
{
public:
    PredictTool(PluginServices &services, QObject *parent) : MapTool(parent), store_(services.store)
    {
        setObjectName(QStringLiteral("predictTool"));
        syncMotion();
        endpoints_.setPaused(store_.motion().paused());
        connect(&endpoints_, &TrackMotion::changed, this, &MapTool::changed);
        connect(&store_.motion(), &TrackMotion::pauseChanged, &endpoints_, &TrackMotion::setPaused);
        connect(&store_.motion(), &TrackMotion::changed, this, [this] {
            if (!records_.isEmpty()) {
                syncMotion();
            }
        });
        connect(&store_, &Store::eventAccepted, this, [this](const Event &event) {
            update(event);
        });
        connect(&store_, &Store::changed, this, [this] {
            if (records_.isEmpty()) {
                return;
            }
            QSet<QString> present;
            for (const auto &event : store_.events()) {
                present.insert(event.uid);
            }
            for (auto it = records_.begin(); it != records_.end(); ++it) {
                if (!present.contains(it.key())) {
                    it.value() = {};
                }
            }
            if (store_.events().isEmpty() && store_.motion().snapshot().isEmpty()) {
                endpoints_.clear();
            }
            emit changed();
        });
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.predict");
    }
    QString settingsTitle() const override
    {
        return tr("Predict");
    }
    QList<SettingField> settingsFields() const override
    {
        return {numberSetting(
                    QStringLiteral("historySeconds"), tr("History window (seconds)"), 10, 600),
                numberSetting(QStringLiteral("opacity"), tr("Prediction opacity"), 0.05, 1.0, 2),
                numberSetting(QStringLiteral("width"), tr("Prediction width"), 0.5, 5, 1)};
    }
    QJsonObject savePreferences() const override
    {
        QJsonArray targets;
        for (auto it = records_.cbegin(); it != records_.cend(); ++it) {
            targets.append(it.key());
        }
        return {{QStringLiteral("historySeconds"), historySeconds_},
                {QStringLiteral("opacity"), opacity_},
                {QStringLiteral("width"), width_},
                {QStringLiteral("targets"), targets}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        historySeconds_ = preferenceInteger(values, QStringLiteral("historySeconds"), 300, 10, 600);
        opacity_ = preferenceNumber(values, QStringLiteral("opacity"), 0.75, 0.05, 1.0);
        width_ = preferenceNumber(values, QStringLiteral("width"), 1.5, 0.5, 5);
        QHash<QString, PredictionHistory> next;
        const auto targets = values.value(QStringLiteral("targets")).toArray();
        // Bound work even when every saved target is invalid or duplicated.
        for (qsizetype i = 0; i < std::min<qsizetype>(targets.size(), MaxPredictions); ++i) {
            const auto uid = targets.at(i).toString();
            if (!uid.isEmpty() && uid.size() <= 128) {
                next.insert(uid, records_.value(uid));
            }
        }
        for (auto it = records_.cbegin(); it != records_.cend(); ++it) {
            if (!next.contains(it.key())) {
                discard(it.key());
            }
        }
        records_ = std::move(next);
        for (const auto &event : store_.events()) {
            update(event);
        }
        emit changed();
        emit actionsChanged();
    }
    bool handle(QEvent &, const Viewport &, QWidget &) override
    {
        return false;
    }
    bool cancel() override
    {
        return false;
    }
    void clear()
    {
        records_.clear();
        endpoints_.clear();
        emit changed();
        emit actionsChanged();
    }
    void populateInformationMenu(const MapItem &item, QMenu &menu) override
    {
        populateContextMenu(item, menu);
    }
    void populateContextMenu(const MapItem &item, QMenu &menu) override
    {
        if (item.kind != MapItem::Kind::Point || item.owner != QStringLiteral("org.swak.events")) {
            return;
        }
        auto *action = menu.addAction(tr("Predict"));
        action->setObjectName(QStringLiteral("predictMarker"));
        action->setCheckable(true);
        action->setChecked(records_.contains(item.id));
        action->setEnabled(action->isChecked() || records_.size() < MaxPredictions);
        action->setToolTip(
            action->isEnabled()
                ? tr("Estimate positions at 1, 10, and 30 minutes from recent CoT history.")
                : tr("The prediction limit is 128 objects. Stop another prediction first."));
        connect(action, &QAction::triggered, this, [this, uid = item.id](bool active) {
            if (!active) {
                records_.remove(uid);
                discard(uid);
            } else if (!records_.contains(uid) && records_.size() < MaxPredictions) {
                records_.insert(uid, {});
                const auto &events = store_.events();
                const auto event =
                    std::find_if(events.cbegin(), events.cend(), [&uid](const Event &entry) {
                        return entry.uid == uid;
                    });
                if (event != events.cend()) {
                    update(*event);
                }
            }
            emit changed();
            emit actionsChanged();
        });
    }
    void paint(QPainter &painter, const Viewport &view) const override
    {
        if (records_.isEmpty()) {
            return;
        }
        QHash<QString, VisualEvent> ends;
        for (const auto &endpoint : endpoints_.snapshot()) {
            ends.insert(endpoint.event.uid, endpoint);
        }
        painter.save();
        painter.setClipRect(QRectF(QPointF{}, view.size));
        const auto scale = view.scale();
        const auto origin =
            QPointF(view.size.width() / 2, view.size.height() / 2) - view.center * scale;
        const auto now = QDateTime::currentDateTimeUtc();
        for (const auto &source : store_.motion().snapshot()) {
            if (!records_.contains(source.event.uid) || source.event.start > now) {
                continue;
            }
            const QColor base = source.event.type.startsWith(QLatin1String("a-h-"))
                                    ? QColor("#ff7a7a")
                                    : QColor("#54c8ff");
            const auto gray = qGray(base.rgb());
            const QColor color(qRound(base.red() * (1 - source.gray) + gray * source.gray),
                               qRound(base.green() * (1 - source.gray) + gray * source.gray),
                               qRound(base.blue() * (1 - source.gray) + gray * source.gray));
            auto previous = source.world;
            double previousOpacity = 1;
            for (qsizetype index = 0; index < static_cast<qsizetype>(Minutes.size()); ++index) {
                const auto found = ends.constFind(
                    endpointId(source.event.uid, Minutes.at(static_cast<size_t>(index))));
                if (found == ends.cend()) {
                    break;
                }
                auto target = found->world;
                target.setX(target.x() - std::round(target.x() - previous.x()));
                painter.setOpacity(opacity_ * source.opacity *
                                   std::min(previousOpacity, found->opacity));
                const auto delta = (target - previous) * scale;
                const auto length = std::hypot(delta.x(), delta.y());
                const auto normal =
                    length > .001 ? QPointF(-delta.y(), delta.x()) / length : QPointF(0, 1);
                const auto tick = normal * (7.0 - static_cast<double>(index) * 2);
                for (int wrap = -1; wrap <= 1; ++wrap) {
                    const auto offset = origin + QPointF(wrap * scale, 0);
                    const auto end = target * scale + offset;
                    painter.setPen(QPen(color, width_, Qt::DashLine, Qt::RoundCap));
                    painter.drawLine(previous * scale + offset, end);
                    painter.setPen(QPen(color, width_, Qt::SolidLine, Qt::RoundCap));
                    painter.drawLine(end - tick, end + tick);
                }
                previous = target;
                previousOpacity = found->opacity;
            }
        }
        painter.restore();
    }

private:
    void syncMotion()
    {
        auto values = store_.motion().savePreferences();
        values.insert(QStringLiteral("predictionMs"), 0);
        values.insert(QStringLiteral("staleSeconds"), 0);
        if (values != motionSettings_) {
            motionSettings_ = values;
            endpoints_.restorePreferences(values);
        }
    }
    void discard(const QString &uid)
    {
        for (const auto minutes : Minutes) {
            endpoints_.remove(endpointId(uid, minutes));
        }
    }
    void expire(const QString &uid)
    {
        for (const auto minutes : Minutes) {
            endpoints_.expire(endpointId(uid, minutes));
        }
    }
    void update(const Event &event)
    {
        auto found = records_.find(event.uid);
        if (found == records_.end()) {
            return;
        }
        syncMotion();
        if (found->size() == 0) {
            discard(event.uid);
        }
        found->append(event,
                      motionSettings_.value(QStringLiteral("teleportKm")).toDouble(100),
                      historySeconds_);
        for (const auto minutes : Minutes) {
            const auto target = found->forecast(minutes * 60);
            if (!target) {
                expire(event.uid);
                break;
            }
            auto endpoint = event;
            endpoint.uid = endpointId(event.uid, minutes);
            const auto coordinate = unproject(*target);
            endpoint.latitude = coordinate.y();
            endpoint.longitude = coordinate.x();
            endpoints_.update(endpoint);
        }
        emit changed();
    }
    Store &store_;
    TrackMotion endpoints_;
    QJsonObject motionSettings_;
    QHash<QString, PredictionHistory> records_;
    int historySeconds_ = 300;
    double opacity_ = 0.75;
    double width_ = 1.5;
};
} // namespace
class PredictPlugin final : public QObject, public Plugin, public MenuPlugin, public MapExtension
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "predict/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MenuPlugin swak::MapExtension)
public:
    QString name() const override
    {
        return tr("Predict");
    }
    QString mapId() const override
    {
        return QStringLiteral("org.swak.predict");
    }
    QString mapName() const override
    {
        return name();
    }
    Kind mapKind() const override
    {
        return Kind::Tool;
    }
    void populateMenu(PluginServices &services, QMenu &menu) override
    {
        if (!services.mapTools) {
            return;
        }
        auto *tool = new PredictTool(services, &menu);
        services.mapTools->add(tool);
        auto *clear = menu.addAction(tr("Clear predictions"));
        clear->setObjectName(QStringLiteral("clearPredictions"));
        connect(clear, &QAction::triggered, tool, [tool] {
            tool->clear();
        });
    }
};
} // namespace swak
#include "predict.moc"

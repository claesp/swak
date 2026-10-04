#include "core/interfaces.h"
#include "core/maps.h"
#include "core/map_tools.h"
#include "core/store.h"
#include "core/user_objects.h"
#include <QMenu>
#include <QPainter>
#include <QJsonArray>
#include <QSet>
#include <cmath>
namespace swak {
namespace {
constexpr int MaxTraces = 128;
QString key(const QString &owner, const QString &id) { return owner + '/' + id; }
class TraceTool final : public MapTool, public SettingsParticipant {
public:
    TraceTool(PluginServices &services, QObject *parent) : MapTool(parent), store_(services.store), objects_(services.objects)
    {
        setObjectName(QStringLiteral("traceTool"));
        connect(&store_, &Store::eventAccepted, this, [this](const Event &event) {
            auto it = records_.find(key(QStringLiteral("org.swak.events"), event.uid));
            if (it != records_.end()) append(*it, project(event.latitude, event.longitude), event.type.startsWith(QLatin1String("a-h-")) ? QColor("#ff7a7a") : QColor("#54c8ff"));
        });
        connect(&store_, &Store::changed, this, [this] { refresh(); });
        if (objects_) connect(objects_, &UserObjects::changed, this, [this] { refresh(); });
    }
    QString preferenceId() const override { return QStringLiteral("org.swak.trace"); }
    QString settingsTitle() const override { return tr("Trace"); }
    QList<SettingField> settingsFields() const override {
        return {numberSetting(QStringLiteral("positions"), tr("Recorded positions"), 2, 50),
            numberSetting(QStringLiteral("opacity"), tr("Trace opacity"), 0.05, 1.0, 2),
            numberSetting(QStringLiteral("width"), tr("Trace width"), 0.5, 5, 1)};
    }
    QJsonObject savePreferences() const override {
        QJsonArray targets;
        for (const auto &record : records_) targets.append(QJsonObject{{QStringLiteral("owner"), record.owner}, {QStringLiteral("id"), record.id}});
        return {{QStringLiteral("styleVersion"), 2}, {QStringLiteral("positions"), limit_}, {QStringLiteral("opacity"), opacity_}, {QStringLiteral("width"), width_}, {QStringLiteral("targets"), targets}};
    }
    void restorePreferences(const QJsonObject &values) override {
        limit_ = preferenceInteger(values, QStringLiteral("positions"), 50, 2, 50);
        opacity_ = preferenceNumber(values, QStringLiteral("opacity"), 0.7, 0.05, 1.0);
        if (values.value(QStringLiteral("styleVersion")).toInt() < 2) {
            if (limit_ == 10) limit_ = 50;
            if (opacity_ == 0.35) opacity_ = 0.7;
        }
        width_ = preferenceNumber(values, QStringLiteral("width"), 1.5, 0.5, 5);
        QHash<QString, Record> next;
        for (const auto &value : values.value(QStringLiteral("targets")).toArray()) {
            if (next.size() >= MaxTraces) break;
            const auto object = value.toObject(); const auto owner = object.value(QStringLiteral("owner")).toString();
            const auto id = object.value(QStringLiteral("id")).toString();
            if (!supported(owner) || id.isEmpty() || id.size() > 128) continue;
            const auto name = key(owner, id); auto record = records_.value(name); record.owner = owner; record.id = id;
            while (record.points.size() > limit_) record.points.removeFirst();
            next.insert(name, record);
        }
        records_ = std::move(next); refresh(); emit changed();
    }
    bool handle(QEvent &, const Viewport &, QWidget &) override { return false; }
    bool cancel() override { return false; }
    void clear() { records_.clear(); emit changed(); }
    void populateContextMenu(const MapItem &item, QMenu &menu) override {
        if (item.kind != MapItem::Kind::Point || !supported(item.owner)) return;
        const auto name = key(item.owner, item.id);
        auto *action = menu.addAction(tr("Trace")); action->setObjectName(QStringLiteral("traceMarker")); action->setCheckable(true);
        action->setChecked(records_.contains(name)); action->setEnabled(records_.contains(name) || records_.size() < MaxTraces);
        if (!action->isEnabled()) action->setToolTip(tr("The trace limit is 128 markers. Clear or stop another trace first."));
        connect(action, &QAction::triggered, this, [this, owner = item.owner, id = item.id, name](bool active) {
            if (!active) records_.remove(name);
            else if (!records_.contains(name) && records_.size() < MaxTraces) records_.insert(name, {owner, id, {}, Qt::white});
            refresh(); emit changed();
        });
    }
    void paint(QPainter &painter, const Viewport &view) const override {
        painter.setClipRect(QRectF(QPointF{}, view.size));
        const auto scale = view.scale(); const auto origin = QPointF(view.size.width() / 2, view.size.height() / 2) - view.center * scale;
        for (const auto &record : records_) for (qsizetype i = 1; i < record.points.size(); ++i) {
            const auto a = record.points.at(i - 1); auto b = record.points.at(i);
            b.setX(a.x() + (b.x() - a.x()) - std::round(b.x() - a.x()));
            auto color = record.color; color.setAlphaF(static_cast<float>(opacity_ * (0.25 + 0.75 * static_cast<double>(i) / static_cast<double>(record.points.size() - 1))));
            painter.setPen(QPen(color, width_, Qt::SolidLine, Qt::RoundCap));
            for (int wrap = -1; wrap <= 1; ++wrap) {
                const auto offset = origin + QPointF(wrap * scale, 0); painter.drawLine(a * scale + offset, b * scale + offset);
            }
        }
    }
private:
    struct Record { QString owner, id; QList<QPointF> points; QColor color; };
    static bool supported(const QString &owner) { return owner == QStringLiteral("org.swak.events") || owner == QStringLiteral("org.swak.markers"); }
    void append(Record &record, QPointF point, QColor color) {
        record.color = color;
        if (!record.points.isEmpty() && record.points.last() == point) return;
        record.points.append(point); if (record.points.size() > limit_) record.points.removeFirst();
    }
    void refresh() {
        if (records_.isEmpty()) return;
        QSet<QString> present;
        for (const auto &event : store_.events()) {
            const auto name = key(QStringLiteral("org.swak.events"), event.uid); auto it = records_.find(name); if (it == records_.end()) continue;
            present.insert(name); append(*it, project(event.latitude, event.longitude), event.type.startsWith(QLatin1String("a-h-")) ? QColor("#ff7a7a") : QColor("#54c8ff"));
        }
        if (objects_) for (const auto &object : objects_->items()) {
            const auto name = key(QStringLiteral("org.swak.markers"), object.id); auto it = records_.find(name); if (it == records_.end() || object.type != QStringLiteral("circle")) continue;
            present.insert(name); append(*it, QPointF(object.geometry.value(QStringLiteral("x")).toDouble(), object.geometry.value(QStringLiteral("y")).toDouble()), QColor(object.color));
        }
        for (auto it = records_.begin(); it != records_.end(); ++it) if (!present.contains(it.key())) it->points.clear();
        emit changed();
    }
    Store &store_;
    UserObjects *objects_;
    QHash<QString, Record> records_;
    int limit_ = 50;
    double opacity_ = 0.7;
    double width_ = 1.5;
};
}
class TracePlugin final : public QObject, public Plugin, public MenuPlugin, public MapExtension {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "trace/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MenuPlugin swak::MapExtension)
public:
    QString name() const override { return tr("Trace"); }
    QString mapId() const override { return QStringLiteral("org.swak.trace"); }
    QString mapName() const override { return name(); }
    Kind mapKind() const override { return Kind::Tool; }
    void populateMenu(PluginServices &services, QMenu &menu) override {
        if (!services.mapTools) return;
        auto *tool = new TraceTool(services, &menu); services.mapTools->add(tool);
        auto *clear = menu.addAction(tr("Clear traces")); clear->setObjectName(QStringLiteral("clearTraces"));
        connect(clear, &QAction::triggered, tool, [tool] { tool->clear(); });
    }
};
}
#include "trace.moc"

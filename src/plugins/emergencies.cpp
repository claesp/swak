#include "core/cot_detail.h"
#include "core/emergency.h"
#include "core/interfaces.h"
#include "core/logging.h"
#include "core/map_label.h"
#include "core/map_tools.h"
#include "core/projection.h"
#include "core/store.h"
#include "core/ui_icons.h"

#include <QMenu>
#include <QPainter>
#include <QSet>
#include <QToolBar>
#include <QToolButton>

#include <memory>

namespace swak {
namespace {
const QColor AlertColor(QStringLiteral("#e53935"));
// The callsign of the person in distress. ATAK puts it in the emergency text.
QString senderCallsign(const Event &event)
{
    if (const auto details = emergency(event); details && !details->text.trimmed().isEmpty()) {
        return details->text.trimmed().left(64);
    }
    auto callsign = event.callsign;
    if (callsign.endsWith(QStringLiteral("-Alert"))) {
        callsign.chop(6);
    }
    return (callsign.isEmpty() ? event.uid : callsign).left(64);
}
QString alertText(const Event &event)
{
    return QStringLiteral("%1 · %2").arg(emergencyLabel(event).left(64), senderCallsign(event));
}
} // namespace
class EmergencyPlugin final : public QObject, public Plugin, public MapLayer, public ToolbarPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "emergencies/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapLayer swak::ToolbarPlugin)
public:
    QString name() const override
    {
        return tr("Emergencies");
    }
    int order() const override
    {
        // Above the CoT symbols, so the alert ring stays visible.
        return 110;
    }
    void paint(QPainter &painter, const Viewport &view, const Store &store) const override
    {
        const QRectF visible(QPointF(0, 0), view.size);
        for (const auto &visual : store.motion().snapshot()) {
            if (visual.expired || !activeEmergency(visual.event)) {
                continue;
            }
            const QPointF point = (visual.world - view.center) * view.scale() +
                                  QPointF(view.size.width() / 2, view.size.height() / 2);
            const double outer = view.markerSize(22);
            if (!visible.adjusted(-outer, -outer, outer, outer).contains(point)) {
                continue;
            }
            painter.save();
            painter.setOpacity(visual.opacity);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(AlertColor, 3));
            painter.drawEllipse(point, view.markerSize(15), view.markerSize(15));
            painter.setPen(QPen(AlertColor, 2, Qt::DashLine));
            painter.drawEllipse(point, outer, outer);
            paintMapLabel(
                painter, point + QPointF(outer + 4, outer), alertText(visual.event), true);
            painter.restore();
        }
    }
    void populateToolbar(PluginServices &services, QToolBar &toolbar, QMenu *) override
    {
        auto *button = new QToolButton(&toolbar);
        button->setObjectName(QStringLiteral("emergencyButton"));
        button->setIcon(uiIcon(UiIcon::Emergency));
        button->setPopupMode(QToolButton::InstantPopup);
        button->setAccessibleName(tr("Emergencies"));
        auto *menu = new QMenu(button);
        menu->setObjectName(QStringLiteral("emergencyMenu"));
        button->setMenu(menu);
        auto *action = toolbar.addWidget(button);
        action->setProperty("swakToolbarTrailing", true);
        // Stay left of the navigation lock, which ends the toolbar.
        action->setProperty("swakToolbarTrailingOrder", -1);
        auto &store = services.store;
        QPointer<MapTools> tools = services.mapTools;
        // UIDs that already have an Events entry. TAK Server repeats active alerts.
        auto known = std::make_shared<QSet<QString>>();
        const auto refresh = [button, &store, known] {
            QSet<QString> active;
            QStringList lines;
            for (const auto &event : store.events()) {
                if (!activeEmergency(event)) {
                    continue;
                }
                active.insert(event.uid);
                lines.append(alertText(event));
                if (!known->contains(event.uid)) {
                    logMessage(Severity::Warning,
                               Urgency::Immediate,
                               QStringLiteral("swak.emergency"),
                               tr("Emergency: %1.").arg(alertText(event)));
                }
            }
            *known = active;
            button->setProperty("activeEmergencies", static_cast<int>(active.size()));
            button->setToolTip(active.isEmpty()
                                   ? tr("No active emergencies")
                                   : tr("Active emergencies:\n%1").arg(lines.join(u'\n')));
            button->setStyleSheet(active.isEmpty()
                                      ? QString{}
                                      : QStringLiteral("QToolButton { background-color: #e53935; "
                                                       "border-radius: 3px; }"));
        };
        connect(&store, &Store::changed, button, refresh);
        connect(menu, &QMenu::aboutToShow, menu, [menu, &store, tools] {
            menu->clear();
            for (const auto &event : store.events()) {
                if (!activeEmergency(event) || menu->actions().size() >= 32) {
                    continue;
                }
                auto *item = menu->addAction(uiIcon(UiIcon::Emergency), alertText(event));
                const auto uid = event.uid;
                const auto world = project(event.latitude, event.longitude);
                connect(item, &QAction::triggered, menu, [&store, tools, uid, world] {
                    if (tools) {
                        emit tools->centerRequested(world);
                    }
                    store.select(uid);
                });
            }
            if (menu->isEmpty()) {
                menu->addAction(tr("No active emergencies"))->setEnabled(false);
            }
        });
        refresh();
    }
};
} // namespace swak
#include "emergencies.moc"

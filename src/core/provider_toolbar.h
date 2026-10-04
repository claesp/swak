#pragma once
#include "core/logging.h"
#include "core/toolbar.h"
#include "core/ui_icons.h"

#include <QMenu>
#include <QToolBar>
#include <QToolButton>
namespace swak {
template <class Sources>
void addProviderToolbar(QToolBar &toolbar,
                        Sources &sources,
                        QAction &manage,
                        const QString &name,
                        const QString &title,
                        UiIcon icon)
{
    auto *button = new QToolButton(&toolbar);
    button->setObjectName(name + QStringLiteral("Button"));
    button->setIcon(uiIcon(icon));
    button->setText(title);
    button->setToolTip(title);
    button->setAccessibleName(title);
    button->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(button);
    menu->setObjectName(name + QStringLiteral("Menu"));
    button->setMenu(menu);
    setToolbarSection(*toolbar.addWidget(button),
                      ToolbarSection::Providers,
                      name.contains(QStringLiteral("server"), Qt::CaseInsensitive) ? 1 : 2);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, &sources, &manage] {
        menu->clear();
        const auto records = sources.sources();
        for (auto it = records.begin(); it != records.end(); ++it) {
            const auto value = it.value().toObject();
            const auto id = it.key();
            auto *action = menu->addAction(value.value(QStringLiteral("name")).toString());
            action->setData(id);
            action->setCheckable(true);
            action->setChecked(value.value(QStringLiteral("enabled")).toBool());
            action->setToolTip(sources.status(id));
            QObject::connect(action, &QAction::triggered, &sources, [&sources, id](bool enabled) {
                auto current = sources.sources().value(id).toObject();
                if (current.isEmpty()) {
                    return;
                }
                current.insert(QStringLiteral("enabled"), enabled);
                const auto error = sources.put(id, current);
                if (!error.isEmpty()) {
                    logMessage(Severity::Error,
                               Urgency::Attention,
                               QStringLiteral("swak.providers"),
                               error);
                }
            });
        }
        menu->addAction(&manage);
    });
}
} // namespace swak

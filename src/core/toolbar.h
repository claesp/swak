#pragma once
#include <QAction>
#include <QToolBar>
#include <algorithm>
namespace swak {
enum class ToolbarSection { Creation = 1, Measurement = 2 };
inline void setToolbarSection(QAction &action, ToolbarSection section, int order)
{
    action.setProperty("swakToolbarSection", static_cast<int>(section));
    action.setProperty("swakToolbarOrder", order);
}
inline void arrangeToolbarSections(QToolBar &toolbar)
{
    QList<QAction *> grouped;
    for (auto *action : toolbar.actions()) if (action->property("swakToolbarSection").isValid()) {
        grouped.append(action); toolbar.removeAction(action);
    }
    std::stable_sort(grouped.begin(), grouped.end(), [](const QAction *a, const QAction *b) {
        const int first = a->property("swakToolbarSection").toInt(), second = b->property("swakToolbarSection").toInt();
        return first != second ? first < second : a->property("swakToolbarOrder").toInt() < b->property("swakToolbarOrder").toInt();
    });
    int section = 0;
    for (auto *action : grouped) {
        const int next = action->property("swakToolbarSection").toInt();
        if (section != next && !toolbar.actions().isEmpty()) toolbar.addSeparator();
        toolbar.addAction(action); section = next;
    }
    bool separator = true;
    for (auto *action : toolbar.actions()) {
        if (action->isSeparator() && separator) { toolbar.removeAction(action); if (action->parent() == &toolbar) delete action; }
        else separator = action->isSeparator();
    }
    if (!toolbar.actions().isEmpty() && toolbar.actions().last()->isSeparator()) {
        auto *action = toolbar.actions().last(); toolbar.removeAction(action); if (action->parent() == &toolbar) delete action;
    }
}
}

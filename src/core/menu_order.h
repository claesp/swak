#pragma once
#include <QApplication>
#include <QCollator>
#include <QMenu>

#include <algorithm>
namespace swak {
inline void sortMenuActions(QMenu &menu)
{
    auto actions = menu.actions();
    actions.removeIf([&menu](QAction *action) {
        if (!action->isSeparator() || action->property("menuTailOrder").toInt() > 0) {
            return false;
        }
        menu.removeAction(action);
        if (action->parent() == &menu && action->associatedObjects().isEmpty()) {
            action->deleteLater();
        }
        return true;
    });
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(
        actions.begin(), actions.end(), [&collator](const QAction *a, const QAction *b) {
            const int aTail = a->property("menuTailOrder").toInt();
            const int bTail = b->property("menuTailOrder").toInt();
            if (aTail != bTail) {
                return aTail < bTail;
            }
            const auto label = [](const QAction *action) {
                auto text = action->text().section('\t', 0, 0);
                text.remove('&');
                return text;
            };
            return collator.compare(label(a), label(b)) < 0;
        });
    for (qsizetype i = 0; i < actions.size(); ++i) {
        const auto current = menu.actions();
        if (current.at(i) != actions.at(i)) {
            menu.insertAction(current.at(i), actions.at(i));
        }
    }
}
class MenuOrder final : public QObject
{
public:
    explicit MenuOrder(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("swakMenuOrder"));
        parent->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (!sorting_ &&
            (event->type() == QEvent::ActionAdded || event->type() == QEvent::ActionChanged)) {
            sorting_ = true;
            if (auto *menu = qobject_cast<QMenu *>(object)) {
                sortMenuActions(*menu);
            }
            sorting_ = false;
        }
        return false;
    }

private:
    bool sorting_ = false;
};
inline void installMenuOrder()
{
    if (!qApp->findChild<QObject *>(QStringLiteral("swakMenuOrder"), Qt::FindDirectChildrenOnly)) {
        new MenuOrder(qApp);
    }
}
} // namespace swak

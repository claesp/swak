#pragma once
#include "core/modal.h"
#include "core/user_objects.h"

#include <QMessageBox>
#include <QPointer>
#include <QPushButton>

namespace swak {
// Asks before an action. objectName names the dialog for tests and accessibility.
inline bool confirmAction(QWidget *parent,
                          const QString &title,
                          const QString &question,
                          const QString &verb,
                          const QString &objectName = QStringLiteral("actionConfirmation"))
{
    // The caller often uses the parent after a true result. Refuse when it is gone.
    const QPointer<QWidget> owner(parent);
    Modal<QMessageBox> dialog(QMessageBox::NoIcon,
                              title,
                              question,
                              QMessageBox::NoButton,
                              parent ? parent->window() : nullptr);
    dialog->setObjectName(objectName);
    dialog->setTextFormat(Qt::PlainText);
    auto *accept = dialog->addButton(verb, QMessageBox::DestructiveRole);
    auto *cancel = dialog->addButton(QMessageBox::Cancel);
    dialog->setDefaultButton(accept);
    dialog->setEscapeButton(cancel);
    dialog->exec();
    return dialog && (owner || !parent) && dialog->clickedButton() == accept;
}
inline bool confirmDeletion(QWidget *parent, const QString &name)
{
    return confirmAction(parent,
                         QObject::tr("Delete"),
                         QObject::tr("Delete %1?").arg(name.left(256)),
                         QObject::tr("Delete"),
                         QStringLiteral("deleteConfirmation"));
}
inline bool deleteObject(UserObjects &objects, const QString &selectedId, QWidget *parent)
{
    // Keep the identity if selection changes while the dialog is open.
    const QString id = selectedId;
    const auto object = objects.find(id);
    if (object && confirmDeletion(parent, object->title)) {
        objects.remove(id);
        return true;
    }
    return false;
}
} // namespace swak

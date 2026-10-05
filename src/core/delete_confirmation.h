#pragma once
#include "core/user_objects.h"

#include <QMessageBox>
#include <QPushButton>

namespace swak {
inline bool
confirmAction(QWidget *parent, const QString &title, const QString &question, const QString &verb)
{
    QMessageBox dialog(QMessageBox::NoIcon,
                       title,
                       question,
                       QMessageBox::NoButton,
                       parent ? parent->window() : nullptr);
    dialog.setObjectName(QStringLiteral("actionConfirmation"));
    dialog.setTextFormat(Qt::PlainText);
    auto *accept = dialog.addButton(verb, QMessageBox::DestructiveRole);
    auto *cancel = dialog.addButton(QMessageBox::Cancel);
    dialog.setDefaultButton(accept);
    dialog.setEscapeButton(cancel);
    dialog.exec();
    return dialog.clickedButton() == accept;
}
inline bool confirmDeletion(QWidget *parent, const QString &name)
{
    QMessageBox dialog(QMessageBox::NoIcon,
                       QObject::tr("Delete"),
                       QObject::tr("Delete %1?").arg(name.left(256)),
                       QMessageBox::NoButton,
                       parent ? parent->window() : nullptr);
    dialog.setObjectName(QStringLiteral("deleteConfirmation"));
    dialog.setTextFormat(Qt::PlainText);
    auto *remove = dialog.addButton(QObject::tr("Delete"), QMessageBox::DestructiveRole);
    auto *cancel = dialog.addButton(QMessageBox::Cancel);
    dialog.setDefaultButton(remove);
    dialog.setEscapeButton(cancel);
    dialog.exec();
    return dialog.clickedButton() == remove;
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

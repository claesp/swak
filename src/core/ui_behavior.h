#pragma once
#include "core/ui_icons.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextEdit>
namespace swak {
class UiBehavior final : public QObject
{
public:
    // The colors of a read-only text field: the disabled text and base colors.
    static QPalette muted(const QPalette &original)
    {
        auto result = original;
        for (auto group : {QPalette::Active, QPalette::Inactive}) {
            result.setColor(
                group, QPalette::Text, original.color(QPalette::Disabled, QPalette::Text));
            result.setColor(
                group, QPalette::Base, original.color(QPalette::Disabled, QPalette::Base));
        }
        return result;
    }
    explicit UiBehavior(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("swakUiBehavior"));
        parent->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::Show && event->type() != QEvent::ReadOnlyChange &&
            event->type() != QEvent::ApplicationPaletteChange) {
            return false;
        }
        auto *widget = qobject_cast<QWidget *>(object);
        if (!widget) {
            return false;
        }
        // A new application palette, such as from a display mode change, replaces the saved
        // colors. Otherwise a read-only field keeps the colors of the old palette.
        if (event->type() == QEvent::ApplicationPaletteChange) {
            if (widget->property("swakEditablePalette").isValid()) {
                widget->setProperty("swakEditablePalette", QApplication::palette(widget));
                widget->setPalette(muted(QApplication::palette(widget)));
            }
            return false;
        }
        bool readOnly = false, textField = false;
        if (const auto *line = qobject_cast<QLineEdit *>(widget)) {
            readOnly = line->isReadOnly();
            textField = true;
        } else if (const auto *plain = qobject_cast<QPlainTextEdit *>(widget)) {
            readOnly = plain->isReadOnly();
            textField = true;
        } else if (const auto *rich = qobject_cast<QTextEdit *>(widget)) {
            readOnly = rich->isReadOnly();
            textField = true;
        }
        if (textField) {
            const auto saved = widget->property("swakEditablePalette");
            if (readOnly) {
                const auto original = saved.isValid() ? saved.value<QPalette>() : widget->palette();
                if (!saved.isValid()) {
                    widget->setProperty("swakEditablePalette", original);
                }
                widget->setPalette(muted(original));
            } else if (saved.isValid()) {
                widget->setPalette(saved.value<QPalette>());
                widget->setProperty("swakEditablePalette", {});
            }
        }
        if (auto *box = qobject_cast<QDialogButtonBox *>(widget)) {
            if (auto *ok = box->button(QDialogButtonBox::Ok)) {
                ok->setIcon(uiIcon(UiIcon::Confirm));
            }
            if (auto *apply = box->button(QDialogButtonBox::Apply)) {
                apply->setIcon(uiIcon(UiIcon::Apply));
            }
        }
        return false;
    }
};
inline void installUiBehavior()
{
    if (!qApp->findChild<QObject *>(QStringLiteral("swakUiBehavior"), Qt::FindDirectChildrenOnly)) {
        new UiBehavior(qApp);
    }
}
} // namespace swak

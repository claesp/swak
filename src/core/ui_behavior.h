#pragma once
#include "core/ui_icons.h"
#include <QApplication>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QPushButton>
namespace swak {
class UiBehavior final : public QObject {
public:
    explicit UiBehavior(QObject *parent) : QObject(parent) { setObjectName(QStringLiteral("swakUiBehavior")); parent->installEventFilter(this); }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() != QEvent::Show && event->type() != QEvent::ReadOnlyChange) return false;
        auto *widget = qobject_cast<QWidget *>(object); if (!widget) return false;
        bool readOnly = false, textField = false;
        if (const auto *line = qobject_cast<QLineEdit *>(widget)) { readOnly = line->isReadOnly(); textField = true; }
        else if (const auto *plain = qobject_cast<QPlainTextEdit *>(widget)) { readOnly = plain->isReadOnly(); textField = true; }
        else if (const auto *rich = qobject_cast<QTextEdit *>(widget)) { readOnly = rich->isReadOnly(); textField = true; }
        if (textField) {
            const auto saved = widget->property("swakEditablePalette");
            if (readOnly) {
                const auto original = saved.isValid() ? saved.value<QPalette>() : widget->palette();
                if (!saved.isValid()) widget->setProperty("swakEditablePalette", original);
                auto muted = original;
                for (auto group : {QPalette::Active, QPalette::Inactive}) {
                    muted.setColor(group, QPalette::Text, original.color(QPalette::Disabled, QPalette::Text));
                    muted.setColor(group, QPalette::Base, original.color(QPalette::Disabled, QPalette::Base));
                }
                widget->setPalette(muted);
            } else if (saved.isValid()) { widget->setPalette(saved.value<QPalette>()); widget->setProperty("swakEditablePalette", {}); }
        }
        if (auto *box = qobject_cast<QDialogButtonBox *>(widget)) {
            if (auto *ok = box->button(QDialogButtonBox::Ok)) ok->setIcon(uiIcon(UiIcon::Confirm));
            if (auto *apply = box->button(QDialogButtonBox::Apply)) apply->setIcon(uiIcon(UiIcon::Apply));
        }
        return false;
    }
};
inline void installUiBehavior()
{
    if (!qApp->findChild<QObject *>(QStringLiteral("swakUiBehavior"), Qt::FindDirectChildrenOnly)) new UiBehavior(qApp);
}
}

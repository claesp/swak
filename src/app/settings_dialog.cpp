#include "app/settings_dialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <utility>

namespace swak {
namespace {
QJsonValue editorValue(QWidget *editor)
{
    if (auto *box = qobject_cast<QCheckBox *>(editor)) return box->isChecked();
    if (auto *number = qobject_cast<QDoubleSpinBox *>(editor)) return number->value();
    if (auto *text = qobject_cast<QLineEdit *>(editor)) return text->text();
    if (auto *choices = qobject_cast<QComboBox *>(editor)) return QJsonValue::fromVariant(choices->currentData());
    return {};
}
}
SettingsDialog::SettingsDialog(QList<SettingsPage> pages, std::function<QString()> save, QWidget *parent)
    : QDialog(parent), pages_(std::move(pages)), save_(std::move(save))
{
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(tr("Settings"));
    resize(760, 580);
    setMinimumSize(600, 400);
    auto *layout = new QVBoxLayout(this);
    auto *body = new QHBoxLayout;
    auto *sections = new QListWidget(this);
    sections->setObjectName(QStringLiteral("settingsSections"));
    sections->setAccessibleName(tr("Settings sections"));
    sections->setMaximumWidth(220);
    auto *stack = new QStackedWidget(this);
    body->addWidget(sections);
    body->addWidget(stack, 1);
    layout->addLayout(body, 1);
    for (const auto &page : pages_) {
        sections->addItem(page.title);
        auto *scroll = new QScrollArea(stack);
        scroll->setWidgetResizable(true);
        auto *content = new QWidget(scroll);
        auto *form = new QFormLayout(content);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setLabelAlignment(Qt::AlignLeft);
        QList<QWidget *> editors;
        if (page.fields.isEmpty()) {
            auto *note = new QLabel(tr("This plugin has no configurable parameters."), content);
            note->setWordWrap(true);
            form->addRow(note);
        }
        for (const auto &field : page.fields) {
            const auto value = settingValue(page.values, field.path);
            QWidget *editor = nullptr;
            switch (field.kind) {
            case SettingField::Kind::Boolean: {
                auto *box = new QCheckBox(field.label, content);
                box->setChecked(value.toBool());
                editor = box;
                break;
            }
            case SettingField::Kind::Number: {
                auto *box = new QDoubleSpinBox(content);
                box->setDecimals(field.decimals);
                box->setRange(field.minimum, field.maximum);
                box->setValue(value.toDouble());
                editor = box;
                break;
            }
            case SettingField::Kind::Text: {
                auto *text = new QLineEdit(content);
                text->setMaxLength(static_cast<int>(field.maximum));
                text->setText(value.toString());
                editor = text;
                break;
            }
            case SettingField::Kind::Choice: {
                auto *choices = new QComboBox(content);
                for (int i = 0; i < field.choices.size(); ++i) {
                    choices->addItem(field.choices.at(i), field.choiceValues.at(i).toVariant());
                    if (field.choiceValues.at(i) == value) choices->setCurrentIndex(i);
                }
                editor = choices;
                break;
            }
            }
            editor->setObjectName(page.id + '/' + field.path);
            editor->setAccessibleName(field.label);
            editor->setProperty("initialSettingValue", editorValue(editor).toVariant());
            if (field.kind == SettingField::Kind::Boolean) form->addRow(editor);
            else {
                auto *label = new QLabel(field.label, content);
                label->setTextFormat(Qt::PlainText);
                label->setWordWrap(true);
                label->setBuddy(editor);
                form->addRow(label, editor);
            }
            editors.append(editor);
        }
        editors_.append(editors);
        scroll->setWidget(content);
        stack->addWidget(scroll);
    }
    connect(sections, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
    sections->setCurrentRow(0);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("settingsStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("settingsButtons"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { if (applyChanges()) accept(); });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { applyChanges(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
bool SettingsDialog::applyChanges()
{
    QList<QJsonObject> values;
    for (qsizetype i = 0; i < pages_.size(); ++i) {
        auto updated = QJsonValue(pages_.at(i).values);
        for (qsizetype j = 0; j < pages_.at(i).fields.size(); ++j) {
            auto *editor = editors_.at(i).at(j);
            const auto value = editorValue(editor);
            if (value != QJsonValue::fromVariant(editor->property("initialSettingValue")))
                updated = withSettingValue(updated, pages_.at(i).fields.at(j).path, value);
        }
        const auto object = updated.toObject();
        if (pages_.at(i).validate && object != pages_.at(i).values) {
            const auto error = pages_.at(i).validate(object);
            if (!error.isEmpty()) { status_->setText(pages_.at(i).title + QStringLiteral(": ") + error); return false; }
        }
        values.append(object);
    }
    for (qsizetype i = 0; i < pages_.size(); ++i) {
        auto &page = pages_[i];
        if (values.at(i) != page.values && page.apply) page.apply(values.at(i));
        page.values = values.at(i);
        for (auto *editor : editors_.at(i)) editor->setProperty("initialSettingValue", editorValue(editor).toVariant());
    }
    const auto error = save_();
    status_->setText(error.isEmpty() ? tr("Configuration saved.") : tr("Changes apply to this session, but saving failed: %1").arg(error));
    return error.isEmpty();
}
}

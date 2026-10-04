#include "core/settings_dialog.h"
#include "core/logging.h"
#include "core/single_line_label.h"
#include <QCheckBox>
#include <QCollator>
#include <QTabWidget>
#include <algorithm>
#include <array>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QKeySequenceEdit>
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
    if (auto *key = qobject_cast<QKeySequenceEdit *>(editor)) return key->keySequence().toString(QKeySequence::PortableText);
    if (auto *text = qobject_cast<QLineEdit *>(editor)) return text->text();
    if (auto *choices = qobject_cast<QComboBox *>(editor)) return QJsonValue::fromVariant(choices->currentData());
    return {};
}
}
SettingsDialog::SettingsDialog(QList<SettingsPage> pages, std::function<QString()> save, QWidget *parent, bool providerForm, bool newProvider)
    : QDialog(parent), pages_(std::move(pages)), save_(std::move(save)), newProvider_(newProvider)
{
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(tr("Settings"));
    resize(760, 580);
    setMinimumSize(600, 400);
    auto *layout = new QVBoxLayout(this);
    const QCollator collator;
    std::stable_sort(pages_.begin(), pages_.end(), [&collator](const SettingsPage &a, const SettingsPage &b) {
        return collator.compare(a.title, b.title) < 0;
    });
    QTabWidget *tabs = providerForm ? nullptr : new QTabWidget(this);
    if (tabs) {
        tabs->setObjectName(QStringLiteral("settingsTabs"));
        layout->addWidget(tabs, 1);
    }
    std::array<QListWidget *, 3> lists{};
    std::array<QStackedWidget *, 3> stacks{};
    if (providerForm) {
        auto *stack = new QStackedWidget(this);
        stacks[0] = stack;
        if (newProvider) {
            auto *choice = new QComboBox(this);
            choice->setObjectName(QStringLiteral("mapProviderFormats"));
            choice->setAccessibleName(tr("Provider format"));
            for (const auto &page : pages_) choice->addItem(page.title);
            providerChoice_ = choice;
            layout->addWidget(choice);
            connect(choice, &QComboBox::currentIndexChanged, stack, &QStackedWidget::setCurrentIndex);
        }
        layout->addWidget(stack, 1);
    } else for (int i = 0; i < 3; ++i) {
        auto *tab = new QWidget(tabs);
        auto *body = new QHBoxLayout(tab);
        auto *sections = new QListWidget(tab);
        sections->setObjectName(i == 0 ? QStringLiteral("settingsSections") : (i == 1 ? QStringLiteral("paneSettingsSections") : QStringLiteral("keybindingSettingsSections")));
        sections->setAccessibleName(i == 0 ? tr("Settings sections") : (i == 1 ? tr("Pane sections") : tr("Keybinding sections")));
        sections->setMaximumWidth(220);
        auto *stack = new QStackedWidget(tab);
        body->addWidget(sections);
        body->addWidget(stack, 1);
        tabs->addTab(tab, i == 0 ? tr("General") : (i == 1 ? tr("Panes") : tr("Keybindings")));
        lists.at(static_cast<size_t>(i)) = sections;
        stacks.at(static_cast<size_t>(i)) = stack;
        connect(sections, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
    }
    for (const auto &page : pages_) {
        const size_t group = !providerForm && page.paneCustomization ? 1 : 0;
        auto addSection = [&](size_t target, QWidget *widget) {
            if (lists.at(target)) {
                lists.at(target)->addItem(page.title);
                lists.at(target)->item(lists.at(target)->count() - 1)->setData(Qt::UserRole, page.id);
            }
            stacks.at(target)->addWidget(widget);
        };
        if (page.createContent) {
            addSection(group, page.createContent(stacks.at(group)));
            editors_.append(QList<QWidget *>{});
            continue;
        }
        std::array<QFormLayout *, 3> forms{};
        auto formFor = [&](size_t target) {
            if (!forms.at(target)) {
                auto *scroll = new QScrollArea(stacks.at(target));
                scroll->setWidgetResizable(true);
                auto *content = new QWidget(scroll);
                auto *form = new QFormLayout(content);
                form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
                form->setRowWrapPolicy(providerForm ? QFormLayout::DontWrapRows : QFormLayout::WrapAllRows);
                form->setLabelAlignment(Qt::AlignLeft);
                scroll->setWidget(content);
                addSection(target, scroll);
                forms.at(target) = form;
            }
            return forms.at(target);
        };
        QList<QWidget *> editors;
        if (page.fields.isEmpty()) {
            auto *form = formFor(group);
            auto *content = form->parentWidget();
            auto *note = new QLabel(tr("This plugin has no configurable parameters."), content);
            note->setWordWrap(false);
            form->addRow(note);
        }
        for (const auto &field : page.fields) {
            auto *form = formFor(!providerForm && field.kind == SettingField::Kind::Key ? 2 : group);
            auto *content = form->parentWidget();
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
            case SettingField::Kind::Key: {
                auto *key = new QKeySequenceEdit(QKeySequence::fromString(value.toString(), QKeySequence::PortableText), content);
                editor = key;
                break;
            }
            case SettingField::Kind::Choice: {
                auto *choices = new QComboBox(content);
                QList<int> order;
                for (int i = 0; i < field.choices.size(); ++i) order.append(i);
                std::stable_sort(order.begin(), order.end(), [&collator, &field](int a, int b) {
                    return collator.compare(field.choices.at(a), field.choices.at(b)) < 0;
                });
                for (const int i : order) {
                    choices->addItem(field.choices.at(i), field.choiceValues.at(i).toVariant());
                    if (field.choiceValues.at(i) == (value.isUndefined() ? field.choiceValues.first() : value)) choices->setCurrentIndex(choices->count() - 1);
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
                label->setWordWrap(false);
                label->setMinimumWidth(label->sizeHint().width());
                label->setBuddy(editor);
                form->addRow(label, editor);
            }
            editors.append(editor);
        }
        editors_.append(editors);
    }
    for (auto *sections : lists) if (sections) sections->setCurrentRow(0);
    status_ = new SingleLineLabel(this);
    status_->setObjectName(QStringLiteral("settingsStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(false);
    layout->addWidget(status_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | (newProvider ? QDialogButtonBox::NoButton : QDialogButtonBox::Apply), this);
    buttons->setObjectName(QStringLiteral("settingsButtons"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { if (applyChanges()) accept(); });
    if (!newProvider) connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { applyChanges(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
QJsonObject SettingsDialog::navigation() const
{
    QJsonObject values;
    if (auto *tabs = findChild<QTabWidget *>()) values.insert(QStringLiteral("tab"), tabs->currentIndex() == 2 ? QStringLiteral("keybindings") : (tabs->currentIndex() == 1 ? QStringLiteral("panes") : QStringLiteral("general")));
    for (auto *list : findChildren<QListWidget *>()) {
        if (list->currentItem()) values.insert(list->objectName(), list->currentItem()->data(Qt::UserRole).toString());
    }
    return values;
}
void SettingsDialog::restoreNavigation(const QJsonObject &values)
{
    if (auto *tabs = findChild<QTabWidget *>()) tabs->setCurrentIndex(values.value(QStringLiteral("tab")).toString() == QStringLiteral("panes") ? 1 : (values.value(QStringLiteral("tab")).toString() == QStringLiteral("keybindings") ? 2 : 0));
    for (auto *list : findChildren<QListWidget *>()) {
        const auto id = values.value(list->objectName()).toString();
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole).toString() == id) { list->setCurrentRow(i); break; }
        }
    }
}
bool SettingsDialog::applyChanges()
{
    const int selected = providerChoice_ ? providerChoice_->currentIndex() : -1;
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
        if ((selected < 0 || i == selected) && pages_.at(i).validate && (newProvider_ || object != pages_.at(i).values)) {
            const auto error = pages_.at(i).validate(object);
            if (!error.isEmpty()) {
                status_->setText(pages_.at(i).title + QStringLiteral(": ") + error);
                logMessage(Severity::Error, Urgency::Attention, QStringLiteral("swak.settings"), status_->text());
                return false;
            }
        }
        values.append(object);
    }
    for (qsizetype i = 0; i < pages_.size(); ++i) {
        if (selected >= 0 && i != selected) continue;
        auto &page = pages_[i];
        if ((newProvider_ || values.at(i) != page.values) && page.apply) page.apply(values.at(i));
        page.values = values.at(i);
        for (auto *editor : editors_.at(i)) editor->setProperty("initialSettingValue", editorValue(editor).toVariant());
    }
    const auto error = save_();
    status_->setText(error.isEmpty() ? tr("Configuration saved.") : tr("Changes apply to this session, but saving failed: %1").arg(error));
    logMessage(error.isEmpty() ? Severity::Info : Severity::Error, error.isEmpty() ? Urgency::Routine : Urgency::Attention,
        QStringLiteral("swak.settings"), status_->text());
    return error.isEmpty();
}
}

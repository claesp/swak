#include "core/cot_picker.h"
#include "core/event.h"
#include <QCoreApplication>
#include <QSignalBlocker>
#include "core/ui_icons.h"
#include <QHash>
#include <QCollator>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
namespace swak {
namespace {
struct Choice { QString code, label; };
using Choices = QList<Choice>;
QString translated(const char *value) { return QCoreApplication::translate("CotPicker", value); }
Choices choices(CotField kind)
{
    Choices result;
    const auto add = [&result](const char *code, const char *label) { result.append({QString::fromLatin1(code), translated(label)}); };
    if (kind == CotField::Type) {
        add("a", "Object"); add("b", "Information"); add("r", "Reservation, restriction, or reference");
        add("t", "Tasking"); add("c", "Capability"); add("y", "Reply");
        const Choices affiliations{{"p", translated("Pending")}, {"u", translated("Unknown")}, {"a", translated("Assumed friend")},
            {"f", translated("Friend")}, {"n", translated("Neutral")}, {"s", translated("Suspect")}, {"h", translated("Hostile")},
            {"j", translated("Joker")}, {"k", translated("Faker")}, {"o", translated("Not specified")}};
        const Choices dimensions{{"P", translated("Space")}, {"A", translated("Air")}, {"G", translated("Ground")},
            {"S", translated("Sea surface")}, {"U", translated("Sea subsurface")}, {"X", translated("Other")},
            {"G-U", translated("Unit")}, {"G-U-C", translated("Combat")}, {"G-U-C-I", translated("Infantry")},
            {"G-E", translated("Equipment")}, {"G-E-V", translated("Vehicle")}, {"G-E-V-A", translated("Armored vehicle")},
            {"G-E-V-C", translated("Civilian vehicle")}, {"G-I", translated("Installation")},
            {"A-C", translated("Civil aircraft")}, {"A-M", translated("Military aircraft")},
            {"A-M-F", translated("Fixed wing")}, {"A-M-H", translated("Rotary wing")}};
        for (const auto &affiliation : affiliations) {
            const auto prefix = QStringLiteral("a-") + affiliation.code;
            result.append({prefix, affiliation.label});
            for (const auto &dimension : dimensions) result.append({prefix + '-' + dimension.code, dimension.label});
        }
        add("b-i", "Imagery"); add("b-i-e", "Electro-optical"); add("b-i-i", "Infrared");
        add("b-i-s", "Synthetic aperture radar"); add("b-i-v", "Video");
        add("b-d", "Sensor detection"); add("b-d-s", "Seismic"); add("b-d-d", "Doppler");
        add("b-d-a", "Acoustic"); add("b-d-m", "Motion");
    } else if (kind == CotField::How) {
        add("h", "Human entered or modified"); add("h-e", "Estimated"); add("h-c", "Calculated");
        add("h-t", "Transcribed"); add("h-p", "Copied and pasted");
        add("m", "Machine generated"); add("m-i", "Measured from imagery"); add("m-g", "GPS receiver");
        add("m-m", "Magnetic source"); add("m-s", "Simulation"); add("m-f", "Combined sources");
        add("m-c", "Configuration file"); add("m-p", "Prediction"); add("m-r", "Relayed from another system");
    } else if (kind == CotField::Team) {
        for (const auto *name : {"White", "Yellow", "Orange", "Magenta", "Red", "Maroon", "Purple", "Dark Blue", "Blue", "Cyan", "Teal", "Green", "Dark Green", "Brown"}) add(name, name);
    } else if (kind == CotField::Role) {
        for (const auto *name : {"Team Member", "Team Lead", "HQ", "Sniper", "Medic", "Forward Observer", "RTO", "K9"}) add(name, name);
    }
    return result;
}
bool valid(CotField kind, const QString &value)
{
    Event event; event.uid = QStringLiteral("picker"); event.type = QStringLiteral("a-f-G"); event.how = QStringLiteral("h-e");
    event.time = event.start = QDateTime::currentDateTimeUtc(); event.stale = event.time.addSecs(60);
    if (kind == CotField::Type) event.type = value;
    if (kind == CotField::How) event.how = value;
    if (kind == CotField::Team) event.team = value;
    if (kind == CotField::Role) event.role = value;
    return eventError(event).isEmpty();
}
class SortedItem final : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem &other) const override { return QCollator().compare(text(0), other.text(0)) < 0; }
};
void openPicker(QLineEdit *field, CotField kind)
{
    auto *dialog = new QDialog(field);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("cotPickerDialog"));
    dialog->setWindowTitle(translated("Select %1").arg(field->accessibleName()));
    dialog->resize(540, 560);
    auto *layout = new QVBoxLayout(dialog);
    auto *tree = new QTreeWidget(dialog); tree->setObjectName(QStringLiteral("cotChoices"));
    tree->setHeaderLabels({translated("Category"), translated("Code")});
    tree->setAccessibleName(translated("Available categories"));
    QHash<QString, QTreeWidgetItem *> items;
    for (const auto &choice : choices(kind)) {
        const auto parentCode = choice.code.left(choice.code.lastIndexOf('-'));
        auto *parent = items.value(parentCode);
        auto *item = parent ? new SortedItem(parent) : new SortedItem(tree);
        item->setText(0, choice.label); item->setText(1, choice.code); item->setData(0, Qt::UserRole, choice.code);
        if (kind == CotField::Team) {
            auto colorName = choice.code; colorName.remove(' '); QColor color(colorName);
            if (color.isValid()) { CollectionSymbol symbol; symbol.frame.addEllipse(QPointF{}, 0.8, 0.8); item->setIcon(0, collectionIcon(symbol, color)); }
        }
        items.insert(choice.code, item);
    }
    tree->sortItems(0, Qt::AscendingOrder);
    tree->setColumnWidth(0, 320);
    layout->addWidget(tree);
    auto *label = new QLabel(translated("Value (custom values are allowed)"), dialog);
    auto *value = new QLineEdit(cotFieldValue(field), dialog); value->setObjectName(QStringLiteral("cotPickerValue"));
    value->setMaxLength(field->property("cotRawMaximum").isValid() ? field->property("cotRawMaximum").toInt() : field->maxLength()); label->setBuddy(value);
    layout->addWidget(label); layout->addWidget(value);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(buttons);
    auto *ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(valid(kind, value->text()));
    QObject::connect(value, &QLineEdit::textChanged, dialog, [kind, ok](const QString &text) { ok->setEnabled(valid(kind, text)); });
    QObject::connect(tree, &QTreeWidget::currentItemChanged, dialog, [value](QTreeWidgetItem *item) { if (item) value->setText(item->data(0, Qt::UserRole).toString()); });
    if (auto *item = items.value(cotFieldValue(field))) {
        for (auto *ancestor = item->parent(); ancestor; ancestor = ancestor->parent()) ancestor->setExpanded(true);
        tree->setCurrentItem(item); tree->scrollToItem(item);
    }
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [dialog, field, value] { setCotFieldValue(field, value->text()); dialog->accept(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    dialog->open();
}
}
QString cotFieldDescription(CotField kind, const QString &code)
{
    if (code.isEmpty()) return translated("Not specified");
    const auto catalog = choices(kind);
    QStringList labels; QString prefix;
    for (const auto &part : code.split('-')) {
        if (!prefix.isEmpty()) prefix += '-';
        prefix += part;
        const auto found = std::find_if(catalog.cbegin(), catalog.cend(), [&prefix](const Choice &choice) { return choice.code == prefix; });
        if (found == catalog.cend()) { labels.append(translated("Unknown category")); break; }
        labels.append(found->label);
    }
    return labels.join(QStringLiteral(" / ")) + QStringLiteral(" (%1)").arg(code);
}
QString cotFieldValue(const QLineEdit *field)
{
    return field->property("cotRawCode").isValid() ? field->property("cotRawCode").toString() : field->text();
}
void setCotFieldValue(QLineEdit *field, const QString &code)
{
    if (!field->property("cotDescriptionKind").isValid()) { field->setText(code); return; }
    field->setProperty("cotRawCode", code);
    const QSignalBlocker blocker(field);
    field->setText(cotFieldDescription(static_cast<CotField>(field->property("cotDescriptionKind").toInt()), code));
}
QWidget *cotFieldEditor(QLineEdit *field, CotField kind, bool descriptive)
{
    if (descriptive) {
        const auto code = field->text(); field->setProperty("cotDescriptionKind", static_cast<int>(kind));
        field->setProperty("cotRawMaximum", field->maxLength()); field->setMaxLength(1024); field->setReadOnly(true);
        QObject::connect(field, &QLineEdit::textChanged, field, [field](const QString &value) { setCotFieldValue(field, value); });
        setCotFieldValue(field, code);
    }
    auto *container = new QWidget(field->parentWidget());
    auto *layout = new QHBoxLayout(container); layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(field, 1);
    auto *button = new QPushButton(translated("Select…"), container);
    button->setObjectName(field->objectName() + QStringLiteral("/select"));
    button->setAccessibleName(translated("Select %1").arg(field->accessibleName()));
    layout->addWidget(button); container->setFocusProxy(field);
    QObject::connect(button, &QPushButton::clicked, field, [field, kind] { openPicker(field, kind); });
    return container;
}
}

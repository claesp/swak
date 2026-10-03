#include "core/object_editor.h"
#include "core/single_line_label.h"
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QHideEvent>
#include <QKeyEvent>
#include <QCollator>
#include <QComboBox>
#include <QButtonGroup>
#include <QToolButton>
#include <QPainter>
#include <algorithm>
namespace swak {
namespace {
class PropertyDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        auto *editor = new QLineEdit(parent); editor->setMaxLength(index.column() == 0 ? 64 : 256); return editor;
    }
};
}
ObjectEditor::ObjectEditor(UserObjects &objects, QWidget *parent) : PaneContent(parent), objects_(objects)
{
    setObjectName(QStringLiteral("objectEditor"));
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Title"), this));
    title_ = new QLineEdit(this); title_->setObjectName(QStringLiteral("objectTitle")); title_->setAccessibleName(tr("Title"));
    connect(title_, &QLineEdit::returnPressed, this, &ObjectEditor::apply);
    title_->setMaxLength(128); title_->setPlaceholderText(tr("New Marker")); layout->addWidget(title_);
    auto font = title_->font(); font.setPointSizeF(font.pointSizeF() > 0 ? font.pointSizeF() * 1.5 : 16); title_->setFont(font);
    layout->addWidget(new QLabel(tr("Type"), this));
    type_ = new QComboBox(this); type_->setObjectName(QStringLiteral("objectType")); type_->setAccessibleName(tr("Type"));
    type_->addItem(tr("Marker"), QStringLiteral("circle")); layout->addWidget(type_);
    layout->addWidget(new QLabel(tr("Color"), this));
    colors_ = new QButtonGroup(this); colors_->setExclusive(true);
    auto *colorRow = new QHBoxLayout; colorRow->setSpacing(4);
    const QStringList names{tr("Red"), tr("Orange"), tr("Yellow"), tr("Green"), tr("Cyan"), tr("Blue"), tr("Indigo"), tr("Magenta")};
    const QStringList values{QStringLiteral("#e53935"), QStringLiteral("#ffc857"), QStringLiteral("#f5e642"), QStringLiteral("#43a047"),
        QStringLiteral("#00bcd4"), QStringLiteral("#1e88e5"), QStringLiteral("#5e35b1"), QStringLiteral("#d81b60")};
    for (qsizetype i = 0; i < values.size(); ++i) {
        auto *button = new QToolButton(this); button->setObjectName(QStringLiteral("objectColor%1").arg(i));
        button->setToolTip(names.at(i)); button->setAccessibleName(names.at(i)); button->setCheckable(true);
        button->setProperty("markerColor", values.at(i));
        QPixmap icon(18, 18); icon.fill(QColor(values.at(i)));
        { QPainter painter(&icon); painter.setPen(Qt::black); painter.drawRect(0, 0, 17, 17); }
        button->setIcon(QIcon(icon)); button->setIconSize(QSize(18, 18)); button->setFixedSize(28, 28);
        colors_->addButton(button); colorRow->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, button] { color_ = button->property("markerColor").toString(); });
    }
    colorRow->addStretch(); layout->addLayout(colorRow);
    layout->addWidget(new QLabel(tr("Description"), this));
    description_ = new QPlainTextEdit(this); description_->setObjectName(QStringLiteral("objectDescription")); description_->setAccessibleName(tr("Description"));
    description_->setMaximumHeight(150); layout->addWidget(description_);
    connect(description_, &QPlainTextEdit::textChanged, this, [this] {
        if (description_->toPlainText().size() <= 2048) return;
        const QSignalBlocker blocker(description_); description_->setPlainText(description_->toPlainText().left(2048));
    });
    properties_ = new QTableWidget(0, 2, this); properties_->setObjectName(QStringLiteral("objectProperties"));
    properties_->setHorizontalHeaderLabels({tr("Property"), tr("Value")}); properties_->verticalHeader()->hide();
    properties_->horizontalHeader()->setStretchLastSection(true); properties_->setItemDelegate(new PropertyDelegate(properties_)); layout->addWidget(properties_, 1);
    auto *actions = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add property"), this); add->setObjectName(QStringLiteral("addObjectProperty")); actions->addWidget(add);
    auto *remove = new QPushButton(tr("Remove property"), this); actions->addWidget(remove); layout->addLayout(actions);
    connect(add, &QPushButton::clicked, this, [this] {
        if (properties_->rowCount() >= 32) return;
        const int row = properties_->rowCount(); properties_->insertRow(row);
        properties_->setItem(row, 0, new QTableWidgetItem); properties_->setItem(row, 1, new QTableWidgetItem);
        properties_->setCurrentCell(row, 0); properties_->editItem(properties_->item(row, 0));
    });
    connect(remove, &QPushButton::clicked, this, [this] { if (properties_->currentRow() >= 0) properties_->removeRow(properties_->currentRow()); });
    status_ = new SingleLineLabel(this); status_->setObjectName(QStringLiteral("objectEditorStatus")); layout->addWidget(status_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this); layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Apply)->setObjectName(QStringLiteral("applyObject"));
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &ObjectEditor::apply);
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { cancel(); });
}
QJsonObject ObjectEditor::editorState() const
{
    return {{QStringLiteral("header"), QString::fromLatin1(properties_->horizontalHeader()->saveState().toBase64())}};
}
void ObjectEditor::restoreEditorState(const QJsonObject &values)
{
    const auto headerBytes = preferenceText(values, QStringLiteral("header"), {}, 16384).toLatin1();
    if (!headerBytes.isEmpty()) properties_->horizontalHeader()->restoreState(QByteArray::fromBase64(headerBytes, QByteArray::AbortOnBase64DecodingErrors));
}
void ObjectEditor::edit(const UserObject &item)
{
    draft_ = item;
    type_->clear(); type_->addItem(item.type == QStringLiteral("circle") ? tr("Marker") : item.type, item.type);
    color_ = item.color;
    colors_->setExclusive(false);
    for (auto *button : colors_->buttons()) button->setChecked(button->property("markerColor").toString().compare(color_, Qt::CaseInsensitive) == 0);
    colors_->setExclusive(true);
    title_->setText(objects_.find(item.id) ? item.title : QString{});
    description_->setPlainText(item.description); properties_->setRowCount(0); status_->clear();
    auto keys = item.properties.keys(); const QCollator collator;
    std::sort(keys.begin(), keys.end(), [&](const auto &a, const auto &b) { return collator.compare(a, b) < 0; });
    for (const auto &key : keys) {
        const int row = properties_->rowCount(); properties_->insertRow(row);
        properties_->setItem(row, 0, new QTableWidgetItem(key)); properties_->setItem(row, 1, new QTableWidgetItem(item.properties.value(key).toString()));
    }
    emit visibilityRequested(true); title_->setFocus();
}
bool ObjectEditor::cancel()
{
    if (!draft_) return false;
    draft_.reset(); emit visibilityRequested(false); emit cancelled(); return true;
}
void ObjectEditor::hideEvent(QHideEvent *event) { PaneContent::hideEvent(event); cancel(); }
void ObjectEditor::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier && cancel()) { event->accept(); return; }
    PaneContent::keyPressEvent(event);
}
void ObjectEditor::apply()
{
    if (!draft_) return;
    auto item = *draft_; item.title = title_->text().trimmed(); if (item.title.isEmpty()) item.title = tr("New Marker");
    item.type = type_->currentData().toString(); item.color = color_;
    item.description = description_->toPlainText(); item.properties = {};
    for (int row = 0; row < properties_->rowCount(); ++row) {
        const auto *name = properties_->item(row, 0); const auto *value = properties_->item(row, 1);
        const auto key = name ? name->text().trimmed() : QString{};
        if (key.isEmpty() || item.properties.contains(key)) { status_->setText(tr("Each property needs a unique name.")); return; }
        item.properties.insert(key, value ? value->text() : QString{});
    }
    const auto error = objects_.put(item); if (!error.isEmpty()) { status_->setText(error); return; }
    draft_.reset(); emit visibilityRequested(false); emit applied(item.id);
}
}

#include "core/interfaces.h"
#include "core/map_selection.h"
#include "core/map_tools.h"
#include "core/store.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCollator>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
namespace swak {
namespace {
void copyValue(const QString &value)
{
    QApplication::clipboard()->setText(value, QClipboard::Clipboard);
    if (QApplication::clipboard()->supportsSelection()) {
        QApplication::clipboard()->setText(value, QClipboard::Selection);
    }
}
class CopyLabel final : public QLabel
{
public:
    using QLabel::QLabel;

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            copyValue(text());
            event->accept();
        } else {
            QLabel::mouseDoubleClickEvent(event);
        }
    }
};
class Description final : public QPlainTextEdit
{
public:
    using QPlainTextEdit::QPlainTextEdit;

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            copyValue(toPlainText());
            event->accept();
        } else {
            QPlainTextEdit::mouseDoubleClickEvent(event);
        }
    }
};
class InformationPane final : public PaneContent, public SettingsParticipant
{
public:
    InformationPane(MapSelection *selection, Store &store, MapTools *tools, QWidget *parent)
        : PaneContent(parent), selection_(selection), tools_(tools), store_(store),
          owner_(parent->window())
    {
        setObjectName(QStringLiteral("informationPane"));
        qApp->installEventFilter(this);
        QTimer::singleShot(0, this, [this, selection] {
            if (!selection || !selection->item()) {
                emit visibilityRequested(false);
            }
        });
        auto *layout = new QVBoxLayout(this);
        title_ = new CopyLabel(this);
        title_->setObjectName(QStringLiteral("informationTitle"));
        auto font = title_->font();
        font.setPointSizeF(font.pointSizeF() > 0 ? font.pointSizeF() * 1.5 : 16);
        font.setBold(true);
        title_->setFont(font);
        title_->setTextFormat(Qt::PlainText);
        title_->setWordWrap(true);
        layout->addWidget(title_);
        type_ = new CopyLabel(this);
        type_->setObjectName(QStringLiteral("informationType"));
        type_->setTextFormat(Qt::PlainText);
        type_->setWordWrap(true);
        layout->addWidget(type_);
        description_ = new Description(this);
        description_->setObjectName(QStringLiteral("informationDescription"));
        description_->setReadOnly(true);
        description_->setPlaceholderText(tr("No description"));
        description_->setMaximumHeight(120);
        layout->addWidget(description_);
        actionsMenu_ = new QMenu(this);
        actionsLayout_ = new QVBoxLayout;
        layout->addLayout(actionsLayout_);
        if (tools_) {
            connect(
                tools_,
                &MapTools::actionsChanged,
                this,
                [this] {
                    refreshActions(true);
                },
                Qt::QueuedConnection);
        }
        properties_ = new QTableWidget(0, 2, this);
        properties_->setObjectName(QStringLiteral("informationProperties"));
        properties_->setHorizontalHeaderLabels({tr("Property"), tr("Value")});
        properties_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        properties_->verticalHeader()->hide();
        properties_->horizontalHeader()->setStretchLastSection(true);
        properties_->setSelectionBehavior(QAbstractItemView::SelectRows);
        layout->addWidget(properties_, 1);
        properties_->setToolTip(tr("Double-click a value to copy it to the clipboard."));
        connect(properties_, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
            if (column == 1 && properties_->item(row, column)) {
                copyValue(properties_->item(row, column)->text());
            }
        });
        if (selection) {
            connect(selection, &MapSelection::changed, this, [this, selection] {
                const auto &item = selection->item();
                title_->setText(item ? item->label : QString{});
                type_->setText(item ? (item->owner == QStringLiteral("org.swak.events")
                                           ? cotFieldDescription(CotField::Type, item->type)
                                           : item->type)
                                    : QString{});
                description_->setPlainText(item ? item->description : QString{});
                properties_->setRowCount(0);
                if (item) {
                    auto keys = item->properties.keys();
                    const QCollator collator;
                    std::sort(keys.begin(), keys.end(), [&](const auto &a, const auto &b) {
                        return collator.compare(a, b) < 0;
                    });
                    for (const auto &key : keys) {
                        const auto value = item->properties.value(key);
                        QString text;
                        if (value.isString()) {
                            text = value.toString();
                            if (item->owner == QStringLiteral("org.swak.events") &&
                                (key == QStringLiteral("Type") || key == QStringLiteral("How"))) {
                                text = cotFieldDescription(
                                    key == QStringLiteral("Type") ? CotField::Type : CotField::How,
                                    text);
                            }
                        } else {
                            auto json =
                                QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
                            text = QString::fromUtf8(json.mid(1, json.size() - 2));
                        }
                        const int row = properties_->rowCount();
                        properties_->insertRow(row);
                        properties_->setItem(row, 0, new QTableWidgetItem(key));
                        properties_->setItem(row, 1, new QTableWidgetItem(text));
                    }
                }
                refreshActions();
                emit visibilityRequested(item.has_value());
            });
        }
    }
    void paneClosed() override
    {
        if (selection_) {
            selection_->clear();
        }
        store_.select({});
    }
    Qt::DockWidgetArea preferredDockArea() const override
    {
        return Qt::RightDockWidgetArea;
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.information");
    }
    QString settingsTitle() const override
    {
        return tr("Information");
    }
    QList<SettingField> settingsFields() const override
    {
        return {
            keySetting(QStringLiteral("dismissKey"), tr("Close Information and clear selection"))};
    }
    QString validateSettings(const QJsonObject &values) const override
    {
        return dismissalKey(values)
                   ? QString{}
                   : tr("Use Esc or one key from F1 through F12 without modifiers.");
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("dismissKey"),
                 QKeySequence(dismissKey_).toString(QKeySequence::PortableText)},
                {QStringLiteral("header"),
                 QString::fromLatin1(properties_->horizontalHeader()->saveState().toBase64())}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        const int key = dismissalKey(values);
        dismissKey_ = key ? key : Qt::Key_Escape;
        const auto bytes = preferenceText(values, QStringLiteral("header"), {}, 16384).toLatin1();
        if (!bytes.isEmpty()) {
            properties_->horizontalHeader()->restoreState(
                QByteArray::fromBase64(bytes, QByteArray::AbortOnBase64DecodingErrors));
        }
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::KeyPress) {
            return PaneContent::eventFilter(object, event);
        }
        if (QApplication::activeModalWidget() || QApplication::activePopupWidget()) {
            return false;
        }
        auto *widget = qobject_cast<QWidget *>(object);
        bool inPane = false;
        while (widget && widget != owner_) {
            inPane = inPane || widget == this;
            widget = widget->parentWidget();
        }
        if (!widget) {
            return false;
        }
        auto *key = static_cast<QKeyEvent *>(event);
        if (inPane && key->modifiers() == Qt::NoModifier &&
            (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && tools_ &&
            tools_->deleteSelection()) {
            key->accept();
            return true;
        }
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier && tools_ &&
            tools_->dismiss()) {
            key->accept();
            return true;
        }
        if (key->key() != dismissKey_ || key->modifiers() != Qt::NoModifier) {
            return false;
        }
        if ((!selection_ || !selection_->item()) && store_.selected().isEmpty() && !isVisible()) {
            return false;
        }
        if (selection_) {
            selection_->clear();
        }
        store_.select({});
        emit visibilityRequested(false);
        key->accept();
        return true;
    }

private:
    void refreshActions(bool force = false)
    {
        const auto identity = selection_ && selection_->item()
                                  ? selection_->item()->owner + '/' + selection_->item()->id + '/' +
                                        QString::number(static_cast<int>(selection_->item()->kind))
                                  : QString{};
        if (!force && identity == actionIdentity_) {
            return;
        }
        actionIdentity_ = identity;
        while (auto *item = actionsLayout_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        actionsMenu_->clear();
        if (!tools_ || !selection_ || !selection_->item()) {
            return;
        }
        tools_->populateInformationMenu(*selection_->item(), *actionsMenu_);
        for (auto *action : actionsMenu_->actions()) {
            if (!action->isCheckable()) {
                continue;
            }
            auto *box = new QCheckBox(action->text(), this);
            box->setObjectName(QStringLiteral("information/") + action->objectName());
            box->setChecked(action->isChecked());
            box->setEnabled(action->isEnabled());
            box->setToolTip(action->toolTip());
            actionsLayout_->addWidget(box);
            connect(box, &QCheckBox::clicked, action, [action] {
                action->trigger();
            });
        }
    }
    static int dismissalKey(const QJsonObject &values)
    {
        const auto text =
            values.value(QStringLiteral("dismissKey")).toString(QStringLiteral("Esc"));
        if (text.isEmpty() || text.size() > 32) {
            return 0;
        }
        const auto key = QKeySequence::fromString(text, QKeySequence::PortableText);
        if (key.count() != 1 || key[0].keyboardModifiers() != Qt::NoModifier) {
            return 0;
        }
        const int value = key[0].key();
        return value == Qt::Key_Escape || (value >= Qt::Key_F1 && value <= Qt::Key_F12) ? value : 0;
    }
    MapSelection *selection_;
    MapTools *tools_;
    Store &store_;
    QWidget *owner_;
    int dismissKey_ = Qt::Key_Escape;
    QString actionIdentity_;
    QMenu *actionsMenu_;
    QVBoxLayout *actionsLayout_;
    CopyLabel *title_;
    CopyLabel *type_;
    Description *description_;
    QTableWidget *properties_;
};
} // namespace
class InformationPlugin final : public QObject, public Plugin, public PanePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "information/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin)
public:
    QString name() const override
    {
        return tr("Information");
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return new InformationPane(services.selection, services.store, services.mapTools, parent);
    }
};
} // namespace swak
#include "information.moc"

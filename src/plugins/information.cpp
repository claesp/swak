#include "core/interfaces.h"
#include "core/map_selection.h"
#include <QLabel>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QApplication>
#include <QClipboard>
#include <QMouseEvent>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCollator>
#include <QTimer>
#include <algorithm>
namespace swak {
namespace {
void copyValue(const QString &value)
{
    QApplication::clipboard()->setText(value, QClipboard::Clipboard);
    if (QApplication::clipboard()->supportsSelection()) QApplication::clipboard()->setText(value, QClipboard::Selection);
}
class CopyLabel final : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) { copyValue(text()); event->accept(); }
        else QLabel::mouseDoubleClickEvent(event);
    }
};
class Description final : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;
protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) { copyValue(toPlainText()); event->accept(); }
        else QPlainTextEdit::mouseDoubleClickEvent(event);
    }
};
class InformationPane final : public PaneContent, public PreferenceParticipant {
public:
    InformationPane(MapSelection *selection, QWidget *parent) : PaneContent(parent)
    {
        setObjectName(QStringLiteral("informationPane"));
        QTimer::singleShot(0, this, [this, selection] {
            if (!selection || !selection->item()) emit visibilityRequested(false);
        });
        auto *layout = new QVBoxLayout(this);
        title_ = new CopyLabel(this); title_->setObjectName(QStringLiteral("informationTitle"));
        auto font = title_->font(); font.setPointSizeF(font.pointSizeF() > 0 ? font.pointSizeF() * 1.5 : 16); font.setBold(true); title_->setFont(font);
        title_->setTextFormat(Qt::PlainText); title_->setWordWrap(true); layout->addWidget(title_);
        type_ = new CopyLabel(this); type_->setObjectName(QStringLiteral("informationType")); type_->setTextFormat(Qt::PlainText); layout->addWidget(type_);
        description_ = new Description(this); description_->setObjectName(QStringLiteral("informationDescription"));
        description_->setReadOnly(true); description_->setPlaceholderText(tr("No description")); description_->setMaximumHeight(120); layout->addWidget(description_);
        properties_ = new QTableWidget(0, 2, this); properties_->setObjectName(QStringLiteral("informationProperties"));
        properties_->setHorizontalHeaderLabels({tr("Property"), tr("Value")}); properties_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        properties_->verticalHeader()->hide(); properties_->horizontalHeader()->setStretchLastSection(true);
        properties_->setSelectionBehavior(QAbstractItemView::SelectRows); layout->addWidget(properties_, 1);
        properties_->setToolTip(tr("Double-click a value to copy it to the clipboard."));
        connect(properties_, &QTableWidget::cellDoubleClicked, this, [this](int row, int column) {
            if (column == 1 && properties_->item(row, column)) copyValue(properties_->item(row, column)->text());
        });
        if (selection) connect(selection, &MapSelection::changed, this, [this, selection] {
            const auto &item = selection->item();
            title_->setText(item ? item->label : QString{}); type_->setText(item ? item->type : QString{});
            description_->setPlainText(item ? item->description : QString{}); properties_->setRowCount(0);
            if (item) {
                auto keys = item->properties.keys(); const QCollator collator;
                std::sort(keys.begin(), keys.end(), [&](const auto &a, const auto &b) { return collator.compare(a, b) < 0; });
                for (const auto &key : keys) {
                    const auto value = item->properties.value(key);
                    QString text;
                    if (value.isString()) text = value.toString();
                    else { auto json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact); text = QString::fromUtf8(json.mid(1, json.size() - 2)); }
                    const int row = properties_->rowCount(); properties_->insertRow(row);
                    properties_->setItem(row, 0, new QTableWidgetItem(key)); properties_->setItem(row, 1, new QTableWidgetItem(text));
                }
            }
            emit visibilityRequested(item.has_value());
        });
    }
    Qt::DockWidgetArea preferredDockArea() const override { return Qt::RightDockWidgetArea; }
    QString preferenceId() const override { return QStringLiteral("org.swak.information"); }
    QJsonObject savePreferences() const override { return {{QStringLiteral("header"), QString::fromLatin1(properties_->horizontalHeader()->saveState().toBase64())}}; }
    void restorePreferences(const QJsonObject &values) override
    {
        const auto bytes = preferenceText(values, QStringLiteral("header"), {}, 16384).toLatin1();
        if (!bytes.isEmpty()) properties_->horizontalHeader()->restoreState(QByteArray::fromBase64(bytes, QByteArray::AbortOnBase64DecodingErrors));
    }
private:
    CopyLabel *title_;
    CopyLabel *type_;
    Description *description_;
    QTableWidget *properties_;
};
}
class InformationPlugin final : public QObject, public Plugin, public PanePlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "information/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin)
public:
    QString name() const override { return tr("Information"); }
    QWidget *create(PluginServices &services, QWidget *parent) override { return new InformationPane(services.selection, parent); }
};
}
#include "information.moc"

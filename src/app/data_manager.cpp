#include "app/data_manager.h"
#include "app/settings_dialog.h"
#include "core/logging.h"
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QCollator>
#include <QSignalBlocker>
#include <QUuid>
#include <algorithm>
namespace swak {
DataManager::DataManager(DataSources &sources, QWidget *parent, std::function<QString()> save)
    : QDialog(parent), sources_(sources), save_(std::move(save))
{
    setObjectName(QStringLiteral("dataProvidersDialog")); setWindowTitle(tr("Data Providers")); resize(700, 460);
    auto *layout = new QVBoxLayout(this);
    auto *notice = new QLabel(tr("Enable feeds to draw KML/KMZ points, lines, and polygons. Each enabled feed refreshes on its own interval. Downloads use HTTPS. Network links and remote icons are not loaded."), this);
    notice->setWordWrap(true); layout->addWidget(notice);
    list_ = new QListWidget(this); list_->setObjectName(QStringLiteral("dataProviderList")); list_->setAccessibleName(tr("Data providers")); layout->addWidget(list_);
    auto *row = new QHBoxLayout;
    auto button = [&](const QString &label, const QString &id) { auto *value = new QPushButton(label, this); value->setObjectName(id); row->addWidget(value); return value; };
    auto *add = button(tr("Add"), QStringLiteral("addDataProvider"));
    auto *editButton = button(tr("Edit"), QStringLiteral("editDataProvider"));
    auto *remove = button(tr("Remove"), QStringLiteral("removeDataProvider"));
    auto *update = button(tr("Refresh now"), QStringLiteral("refreshDataProvider"));
    layout->addLayout(row);
    status_ = new QLabel(this); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); status_->setObjectName(QStringLiteral("dataProviderStatus")); layout->addWidget(status_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(add, &QPushButton::clicked, this, [this] { edit(true); });
    connect(editButton, &QPushButton::clicked, this, [this] { edit(false); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { edit(false); });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { updateStatus(); });
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const auto id = item->data(Qt::UserRole).toString();
        const bool enabled = item->checkState() == Qt::Checked;
        QTimer::singleShot(0, this, [this, id, enabled] {
            auto source = sources_.sources().value(id).toObject();
            if (source.isEmpty()) return;
            source.insert(QStringLiteral("enabled"), enabled);
            const auto error = sources_.put(id, source);
            if (!error.isEmpty()) { refresh(); status_->setText(error); return; }
            this->save();
        });
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (!list_->currentItem()) return;
        sources_.remove(list_->currentItem()->data(Qt::UserRole).toString()); this->save();
    });
    connect(update, &QPushButton::clicked, this, [this] { if (list_->currentItem()) sources_.refresh(list_->currentItem()->data(Qt::UserRole).toString()); });
    connect(&sources_, &DataSources::changed, this, &DataManager::refresh);
    connect(&sources_, &DataSources::contentChanged, this, &DataManager::updateStatus);
    refresh();
}
void DataManager::save()
{
    const auto error = save_ ? save_() : tr("No profile writer is available.");
    if (!error.isEmpty()) { status_->setText(error); logMessage(Severity::Error, Urgency::Attention, QStringLiteral("swak.data"), error); }
}
void DataManager::refresh()
{
    const auto selected = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : QString{};
    const QSignalBlocker blocked(list_); list_->clear();
    const auto entries = sources_.sources(); auto ids = entries.keys(); const QCollator collator;
    std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) { return collator.compare(entries[a].toObject().value(QStringLiteral("name")).toString(), entries[b].toObject().value(QStringLiteral("name")).toString()) < 0; });
    for (const auto &id : ids) {
        const auto source = entries[id].toObject();
        auto *item = new QListWidgetItem(source.value(QStringLiteral("name")).toString(), list_); item->setData(Qt::UserRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable); item->setCheckState(source.value(QStringLiteral("enabled")).toBool() ? Qt::Checked : Qt::Unchecked);
        if (!sources_.provider(source)) { item->setText(item->text() + tr(" (plugin unavailable)")); item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable); }
        if (id == selected) list_->setCurrentItem(item);
    }
    if (!list_->currentItem() && list_->count()) list_->setCurrentRow(0);
    updateStatus();
}
void DataManager::updateStatus()
{
    if (!list_->currentItem()) { status_->clear(); return; }
    const auto id = list_->currentItem()->data(Qt::UserRole).toString();
    const auto source = sources_.sources().value(id).toObject();
    status_->setText(sources_.status(id) + (source.value(QStringLiteral("ignoreTls")).toBool() ? tr(" Certificate checks are disabled.") : QString{}));
}
void DataManager::edit(bool adding)
{
    if (adding && (sources_.sources().size() >= 8 || sources_.providers().isEmpty())) { status_->setText(tr("An available data plugin and fewer than eight feeds are required.")); return; }
    if (!adding && !list_->currentItem()) return;
    const auto id = adding ? QUuid::createUuid().toString(QUuid::WithoutBraces) : list_->currentItem()->data(Qt::UserRole).toString();
    const auto source = sources_.sources().value(id).toObject();
    if (!adding && !sources_.provider(source)) { status_->setText(tr("Install the missing data plugin to edit this entry.")); return; }
    QList<SettingsPage> pages;
    for (auto *provider : sources_.providers()) {
        if (!adding && provider != sources_.provider(source)) continue;
        auto values = adding ? dataDefaults() : source;
        values.insert(QStringLiteral("format"), provider->dataId());
        pages.append({adding ? provider->dataId() : id, provider->dataName(), dataFields(), values, validateDataSource,
            [this, id](const QJsonObject &value) { applyError_ = sources_.put(id, value); }});
    }
    applyError_.clear();
    SettingsDialog dialog(pages,
        [this] { return !applyError_.isEmpty() ? applyError_ : save_ ? save_() : tr("No profile writer is available."); }, this, true, adding);
    dialog.setWindowTitle(adding ? tr("Add data provider") : tr("Edit data provider"));
    if (dialog.exec() == QDialog::Accepted) for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->data(Qt::UserRole).toString() == id) list_->setCurrentRow(i);
}
}

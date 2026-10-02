#include "app/map_manager.h"
#include "app/settings_dialog.h"
#include <QListWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QCollator>
#include <algorithm>
namespace swak {
MapManager::MapManager(MapSources &sources, QWidget *parent) : QDialog(parent), sources_(sources)
{
    setObjectName(QStringLiteral("mapProvidersDialog"));
    setWindowTitle(tr("Map Providers"));
    resize(650, 460);
    auto *layout = new QVBoxLayout(this);
    auto *notice = new QLabel(tr("Select a provider to fetch visible map tiles over HTTPS. Providers receive your IP address and viewed area. Attribution stays visible on the map. No bulk downloads."), this);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("mapProviderList"));
    list_->setAccessibleName(tr("Map providers"));
    layout->addWidget(list_);
    auto *row = new QHBoxLayout;
    formats_ = new QComboBox(this);
    formats_->setObjectName(QStringLiteral("mapProviderFormats"));
    formats_->setAccessibleName(tr("Provider format"));
    auto providers = sources_.providers();
    const QCollator collator;
    std::sort(providers.begin(), providers.end(), [&collator](const auto *a, const auto *b) { return collator.compare(a->mapName(), b->mapName()) < 0; });
    for (const auto *provider : providers) formats_->addItem(provider->mapName(), provider->mapId());
    row->addWidget(formats_, 1);
    auto *add = new QPushButton(tr("Add"), this);
    add->setObjectName(QStringLiteral("addMapProvider"));
    row->addWidget(add);
    auto *editButton = new QPushButton(tr("Edit"), this);
    editButton->setObjectName(QStringLiteral("editMapProvider"));
    row->addWidget(editButton);
    auto *remove = new QPushButton(tr("Remove"), this);
    remove->setObjectName(QStringLiteral("removeMapProvider"));
    row->addWidget(remove);
    auto *activate = new QPushButton(tr("Use provider"), this);
    activate->setObjectName(QStringLiteral("useMapProvider"));
    row->addWidget(activate);
    layout->addLayout(row);
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *offline = buttons->addButton(tr("Use offline map"), QDialogButtonBox::ActionRole);
    offline->setObjectName(QStringLiteral("offlineMap"));
    connect(offline, &QPushButton::clicked, this, [this] { sources_.select({}); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
    connect(&sources_, &MapSources::changed, this, &MapManager::refresh);
    connect(add, &QPushButton::clicked, this, [this] {
        const auto id = sources_.add(formats_->currentData().toString());
        for (int i = 0; i < list_->count(); ++i) if (list_->item(i)->data(Qt::UserRole).toString() == id) list_->setCurrentRow(i);
        if (!id.isEmpty()) edit();
        else status_->setText(tr("No provider was added. Select an available format and keep at most 32 entries."));
    });
    connect(editButton, &QPushButton::clicked, this, &MapManager::edit);
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { edit(); });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (list_->currentItem()) sources_.remove(list_->currentItem()->data(Qt::UserRole).toString());
    });
    connect(activate, &QPushButton::clicked, this, [this] {
        if (!list_->currentItem()) return;
        const auto id = list_->currentItem()->data(Qt::UserRole).toString();
        const auto source = sources_.sources().value(id).toObject();
        const auto *provider = sources_.provider(source);
        if (!provider) { status_->setText(tr("This provider plugin is unavailable. Its configuration is preserved.")); return; }
        const auto error = provider->validate(source);
        if (!error.isEmpty()) { status_->setText(error); return; }
        sources_.select(id);
    });
    refresh();
}
void MapManager::refresh()
{
    const auto selected = list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : sources_.activeId();
    list_->clear();
    const auto entries = sources_.sources();
    QStringList ids = entries.keys();
    const QCollator collator;
    std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) {
        return collator.compare(entries.value(a).toObject().value(QStringLiteral("name")).toString(), entries.value(b).toObject().value(QStringLiteral("name")).toString()) < 0;
    });
    for (const auto &id : ids) {
        const auto source = entries.value(id).toObject();
        auto *item = new QListWidgetItem(source.value(QStringLiteral("name")).toString(), list_);
        item->setData(Qt::UserRole, id);
        if (!sources_.provider(source)) item->setText(item->text() + tr(" (plugin unavailable)"));
        if (id == selected) list_->setCurrentItem(item);
    }
    if (!list_->currentItem() && list_->count()) list_->setCurrentRow(0);
    const auto source = sources_.activeSource();
    status_->setText(source.isEmpty() ? tr("Offline reference map is active.") : tr("Active: %1%2").arg(source.value(QStringLiteral("name")).toString(),
        source.value(QStringLiteral("ignoreTls")).toBool() ? tr(" | Certificate checks are disabled.") : QString{}));
}
void MapManager::edit()
{
    if (!list_->currentItem()) return;
    const auto id = list_->currentItem()->data(Qt::UserRole).toString();
    const auto source = sources_.sources().value(id).toObject();
    auto *provider = sources_.provider(source);
    if (!provider) { status_->setText(tr("Install the missing provider plugin to edit this entry.")); return; }
    SettingsDialog dialog({{id, source.value(QStringLiteral("name")).toString(), provider->fields(), source,
        [provider](const QJsonObject &values) { return provider->validate(values); },
        [this, id](const QJsonObject &values) { sources_.put(id, values); }}}, [] { return QString{}; }, this);
    dialog.exec();
}
}

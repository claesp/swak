#include "app/map_manager.h"
#include "core/logging.h"
#include "app/settings_dialog.h"
#include "app/provider_table.h"
#include <QTimer>
#include <QLabel>
#include "core/single_line_label.h"
#include <QPushButton>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QCollator>
#include <algorithm>
#include <QUuid>
#include <QLineEdit>
namespace swak {
MapManager::MapManager(MapSources &sources, QWidget *parent, std::function<QString()> save, bool embedded)
    : QDialog(parent), sources_(sources), save_(std::move(save))
{
    if (embedded) setWindowFlags(Qt::Widget);
    setObjectName(QStringLiteral("mapProvidersDialog"));
    setWindowTitle(tr("Map Providers"));
    resize(650, 460);
    auto *layout = new QVBoxLayout(this);
    auto *notice = new SingleLineLabel(tr("Select a provider to fetch visible map tiles over HTTPS. Providers receive your IP address and viewed area. Attribution stays visible on the map. No bulk downloads. Provider changes apply immediately."), this);
    notice->setWordWrap(false);
    layout->addWidget(notice);
    list_ = new ProviderTable(true, this);
    list_->setObjectName(QStringLiteral("mapProviderList"));
    list_->setAccessibleName(tr("Map providers"));
    layout->addWidget(list_);
    auto *row = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add"), this);
    add->setObjectName(QStringLiteral("addMapProvider"));
    row->addWidget(add);
    auto *editButton = new QPushButton(tr("Edit"), this);
    editButton->setObjectName(QStringLiteral("editMapProvider"));
    row->addWidget(editButton);
    auto *remove = new QPushButton(tr("Delete"), this);
    remove->setObjectName(QStringLiteral("removeMapProvider"));
    row->addWidget(remove);
    auto *credentials = new QPushButton(tr("Credentials"), this);
    credentials->setObjectName(QStringLiteral("mapCredentials")); row->addWidget(credentials);
    connect(credentials, &QPushButton::clicked, this, [this] {
        const auto id = list_->selectedId(); const auto source = sources_.sources().value(id).toObject();
        if (id.isEmpty() || !authenticatedMap(source)) { reportError(tr("Select an authentication method in Edit first.")); return; }
        QDialog dialog(this); dialog.setObjectName(QStringLiteral("mapCredentialsDialog")); dialog.setWindowTitle(tr("Map credentials"));
        auto *credentialLayout = new QVBoxLayout(&dialog);
        auto *credentialNotice = new QLabel(tr("Enter the password or token for this session. An empty value clears the session secret. A configured environment variable then applies."), &dialog);
        credentialNotice->setWordWrap(true); credentialLayout->addWidget(credentialNotice);
        auto *secret = new QLineEdit(&dialog); secret->setEchoMode(QLineEdit::Password); secret->setMaxLength(4096);
        secret->setObjectName(QStringLiteral("mapSecret")); secret->setAccessibleName(tr("Password or token")); credentialLayout->addWidget(secret);
        auto *error = new SingleLineLabel(&dialog); credentialLayout->addWidget(error);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); credentialLayout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            const auto problem = sources_.setCredential(id, secret->text().toUtf8());
            if (!problem.isEmpty()) { error->setText(problem); return; }
            secret->clear(); dialog.accept();
        });
        dialog.exec();
    });
    layout->addLayout(row);
    status_ = new SingleLineLabel(this);
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(false);
    layout->addWidget(status_);
    auto *clearProvider = new QPushButton(tr("Clear selection"), this);
    clearProvider->setObjectName(QStringLiteral("clearMapProvider"));
    row->addWidget(clearProvider);
    connect(clearProvider, &QPushButton::clicked, this, [this] { sources_.select({}); });
    if (!embedded) {
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        layout->addWidget(buttons);
    }
    connect(&sources_, &MapSources::changed, this, &MapManager::refresh);
    connect(add, &QPushButton::clicked, this, [this] {
        if (sources_.sources().size() >= 32 || sources_.providers().isEmpty()) {
            reportError(tr("No provider was added. An available format and fewer than 32 entries are required."));
            return;
        }
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QList<SettingsPage> pages;
        for (auto *provider : sources_.providers()) {
            auto values = provider->defaults();
            values.insert(QStringLiteral("format"), provider->mapId());
            for (const auto &field : provider->fields())
                if (field.kind == SettingField::Kind::Text) values.insert(field.path, QString{});
            pages.append({provider->mapId(), provider->mapName(), provider->fields(), values,
                [provider](const QJsonObject &value) { return provider->validate(value); },
                [this, id](const QJsonObject &value) { applyError_ = sources_.put(id, value); }});
        }
        applyError_.clear();
        SettingsDialog dialog(pages, [this] { return !applyError_.isEmpty() ? applyError_ : save_ ? save_() : tr("No profile writer is available."); }, this, true, true);
        dialog.setWindowTitle(tr("Add provider"));
        if (dialog.exec() == QDialog::Accepted)
            for (int i = 0; i < list_->rowCount(); ++i)
                if (list_->item(i, 1)->data(Qt::UserRole).toString() == id) list_->setCurrentCell(i, 1);
    });
    connect(editButton, &QPushButton::clicked, this, &MapManager::edit);
    connect(list_, &QTableWidget::cellDoubleClicked, this, [this](int, int column) { if (column != 0) edit(); });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (!list_->selectedId().isEmpty()) sources_.remove(list_->selectedId());
    });
    refresh();
}
void MapManager::reportError(const QString &message)
{
    status_->setText(message);
    logMessage(Severity::Error, Urgency::Attention, QStringLiteral("swak.maps"), message);
}
void MapManager::refresh()
{
    const auto selected = !list_->selectedId().isEmpty() ? list_->selectedId() : sources_.activeId();
    list_->clearProviders();
    const auto entries = sources_.sources();
    QStringList ids = entries.keys();
    const QCollator collator;
    std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) {
        return collator.compare(entries.value(a).toObject().value(QStringLiteral("name")).toString(), entries.value(b).toObject().value(QStringLiteral("name")).toString()) < 0;
    });
    for (const auto &id : ids) {
        const auto source = entries.value(id).toObject();
        const auto *provider = sources_.provider(source);
        const bool available = provider && provider->validate(source).isEmpty();
        auto *button = list_->append(id, source.value(QStringLiteral("name")).toString(),
            provider ? provider->mapName() : tr("Plugin unavailable"), source.value(QStringLiteral("endpoint")).toString(),
            id == sources_.activeId() && sources_.networkEnabled(), available);
        connect(button, &QAbstractButton::clicked, this, [this, id] {
            // Refresh can replace the sender, so apply changes after its click returns.
            QTimer::singleShot(0, this, [this, id] {
                list_->selectId(id);
                const auto current = sources_.sources().value(id).toObject();
                const auto *format = sources_.provider(current);
                if (!format || !format->validate(current).isEmpty()) { refresh(); return; }
                sources_.select(id);
            });
        });
    }
    list_->selectId(selected);
    if (list_->currentRow() < 0 && list_->rowCount()) list_->setCurrentCell(0, 1);
    const auto source = sources_.activeSource();
    status_->setText(source.isEmpty() ? tr("No map provider selected.") : tr("Active: %1%2").arg(source.value(QStringLiteral("name")).toString(),
        source.value(QStringLiteral("ignoreTls")).toBool() ? tr(" | Certificate checks are disabled.") : QString{}));
}
void MapManager::edit()
{
    const auto id = list_->selectedId();
    if (id.isEmpty()) return;
    const auto source = sources_.sources().value(id).toObject();
    auto *provider = sources_.provider(source);
    if (!provider) { reportError(tr("Install the missing provider plugin to edit this entry.")); return; }
    applyError_.clear();
    SettingsDialog dialog({{id, source.value(QStringLiteral("name")).toString(), provider->fields(), source,
        [provider](const QJsonObject &values) { return provider->validate(values); },
        [this, id](const QJsonObject &values) { applyError_ = sources_.put(id, values); }}},
        [this] { return !applyError_.isEmpty() ? applyError_ : save_ ? save_() : tr("No profile writer is available."); }, this, true);
    dialog.setWindowTitle(tr("Edit provider"));
    dialog.exec();
}
}

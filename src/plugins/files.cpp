#include "core/contacts.h"
#include "core/delete_confirmation.h"
#include "core/file_shares.h"
#include "core/interfaces.h"
#include "core/modal.h"
#include "core/preferences.h"
#include "core/servers.h"
#include "core/single_line_label.h"
#include "core/tak_server_api.h"
#include "core/text.h"
#include "core/user_objects.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace swak {
namespace {
QString sizeText(qint64 size)
{
    return size < 0 ? QObject::tr("Unknown") : QLocale().formattedDataSize(size);
}
QTableWidget *table(const QString &name, const QStringList &headers, QWidget *parent)
{
    auto *result = new QTableWidget(0, static_cast<int>(headers.size()), parent);
    result->setObjectName(name);
    result->setHorizontalHeaderLabels(headers);
    result->setSelectionBehavior(QAbstractItemView::SelectRows);
    result->setSelectionMode(QAbstractItemView::SingleSelection);
    result->setEditTriggers(QAbstractItemView::NoEditTriggers);
    result->verticalHeader()->hide();
    result->horizontalHeader()->setStretchLastSection(true);
    return result;
}
QTableWidgetItem *cell(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setToolTip(plainToolTip(text));
    return item;
}
class FilesPane final : public QWidget, public PreferenceParticipant
{
public:
    FilesPane(PluginServices &services, QWidget *parent)
        : QWidget(parent), files_(services.files), servers_(services.servers),
          contacts_(services.contacts), objects_(services.objects)
    {
        setObjectName(QStringLiteral("filesPane"));
        auto *layout = new QVBoxLayout(this);
        tabs_ = new QTabWidget(this);
        tabs_->setObjectName(QStringLiteral("filesTabs"));
        layout->addWidget(tabs_);
        // Received offers.
        auto *received = new QWidget(tabs_);
        auto *receivedLayout = new QVBoxLayout(received);
        offers_ = table(QStringLiteral("fileOffers"),
                        {tr("Name"), tr("From"), tr("Size"), tr("Status")},
                        received);
        receivedLayout->addWidget(offers_);
        auto *receivedButtons = new QHBoxLayout;
        auto *download = new QPushButton(tr("Download ..."), received);
        download->setObjectName(QStringLiteral("downloadOffer"));
        auto *dismiss = new QPushButton(tr("Dismiss"), received);
        dismiss->setObjectName(QStringLiteral("dismissOffer"));
        auto *send = new QPushButton(tr("Send Package ..."), received);
        send->setObjectName(QStringLiteral("sendPackage"));
        receivedButtons->addWidget(download);
        receivedButtons->addWidget(dismiss);
        receivedButtons->addStretch();
        receivedButtons->addWidget(send);
        receivedLayout->addLayout(receivedButtons);
        tabs_->addTab(received, tr("Received"));
        // Server files.
        auto *server = new QWidget(tabs_);
        auto *serverLayout = new QVBoxLayout(server);
        auto *serverRow = new QHBoxLayout;
        serverChoice_ = new QComboBox(server);
        serverChoice_->setObjectName(QStringLiteral("fileServer"));
        serverChoice_->setAccessibleName(tr("TAK Server"));
        auto *search = new QPushButton(tr("Search"), server);
        search->setObjectName(QStringLiteral("searchServerFiles"));
        serverRow->addWidget(serverChoice_, 1);
        serverRow->addWidget(search);
        serverLayout->addLayout(serverRow);
        serverFiles_ = table(QStringLiteral("serverFiles"),
                             {tr("Name"), tr("Submitter"), tr("Size"), tr("Time")},
                             server);
        serverLayout->addWidget(serverFiles_);
        auto *serverButtons = new QHBoxLayout;
        auto *fetch = new QPushButton(tr("Download ..."), server);
        fetch->setObjectName(QStringLiteral("downloadServerFile"));
        auto *remove = new QPushButton(tr("Delete ..."), server);
        remove->setObjectName(QStringLiteral("deleteServerFile"));
        serverButtons->addWidget(fetch);
        serverButtons->addWidget(remove);
        serverButtons->addStretch();
        serverLayout->addLayout(serverButtons);
        tabs_->addTab(server, tr("Server"));
        status_ = new SingleLineLabel({}, this);
        status_->setObjectName(QStringLiteral("filesStatus"));
        status_->hide();
        layout->addWidget(status_);
        if (!files_) {
            setEnabled(false);
            return;
        }
        connect(files_, &FileShares::changed, this, &FilesPane::showOffers);
        connect(download, &QPushButton::clicked, this, &FilesPane::downloadOffer);
        connect(dismiss, &QPushButton::clicked, this, [this] {
            if (const auto *offer = selectedOffer()) {
                files_->dismiss(offer->serverId, offer->hash);
            }
        });
        connect(send, &QPushButton::clicked, this, &FilesPane::sendPackage);
        connect(search, &QPushButton::clicked, this, [this] {
            const auto id = serverChoice_->currentData().toString();
            if (id.isEmpty()) {
                showStatus(tr("Add a TAK Server entry with a REST API port."));
                return;
            }
            showStatus(tr("Reading the server file list…"));
            files_->search(id);
        });
        connect(files_,
                &FileShares::searched,
                this,
                [this](const QString &id, const QList<ServerFile> &list, const QString &error) {
                    searchedServer_ = id;
                    serverList_ = list;
                    showServerFiles();
                    showStatus(error.isEmpty()
                                   ? tr("%n files.", nullptr, static_cast<int>(list.size()))
                                   : error);
                });
        connect(fetch, &QPushButton::clicked, this, &FilesPane::downloadServerFile);
        connect(remove, &QPushButton::clicked, this, &FilesPane::deleteServerFile);
        connect(files_,
                &FileShares::removed,
                this,
                [this](const QString &id, const QString &hash, const QString &error) {
                    if (error.isEmpty() && id == searchedServer_) {
                        serverList_.removeIf([&hash](const ServerFile &file) {
                            return file.hash == hash;
                        });
                        showServerFiles();
                    }
                    showStatus(error.isEmpty() ? tr("The file is deleted.") : error);
                });
        connect(files_, &FileShares::sent, this, [this](const QString &error) {
            showStatus(error.isEmpty() ? tr("The package is uploaded and offered.") : error);
        });
        if (servers_) {
            connect(servers_, &Servers::changed, this, &FilesPane::showServers);
        }
        showServers();
        showOffers();
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.files");
    }
    QJsonObject savePreferences() const override
    {
        return {{QStringLiteral("tab"), tabs_->currentIndex()}};
    }
    void restorePreferences(const QJsonObject &values) override
    {
        tabs_->setCurrentIndex(preferenceInteger(values, QStringLiteral("tab"), 0, 0, 1));
    }

private:
    void showStatus(const QString &text)
    {
        status_->setText(text);
        status_->setVisible(!text.isEmpty());
    }
    QString serverName(const QString &id) const
    {
        const auto name =
            servers_
                ? servers_->sources().value(id).toObject().value(QStringLiteral("name")).toString()
                : QString{};
        return name.isEmpty() ? id : name;
    }
    void showServers()
    {
        const auto current = serverChoice_->currentData().toString();
        serverChoice_->clear();
        if (!servers_) {
            return;
        }
        const auto sources = servers_->sources();
        for (auto it = sources.begin(); it != sources.end(); ++it) {
            if (!TakServerApi::martiUrl(it.value().toObject(), QStringLiteral("/sync/search"))
                     .isEmpty()) {
                serverChoice_->addItem(serverName(it.key()), it.key());
            }
        }
        const auto index = serverChoice_->findData(current);
        if (index >= 0) {
            serverChoice_->setCurrentIndex(index);
        }
    }
    void showOffers()
    {
        const auto previous = selectedOffer() ? selectedOffer()->hash : QString{};
        const auto &offers = files_->offers();
        offers_->setRowCount(static_cast<int>(offers.size()));
        for (int row = 0; row < offers.size(); ++row) {
            const auto &offer = offers.at(row);
            auto *name = cell(offer.name);
            name->setData(Qt::UserRole, row);
            offers_->setItem(row, 0, name);
            offers_->setItem(row, 1, cell(offer.senderCallsign));
            offers_->setItem(row, 2, cell(sizeText(offer.size)));
            const auto state = offer.state == FileOffer::State::Offered
                                   ? tr("Offered on %1").arg(serverName(offer.serverId))
                                   : offer.status;
            offers_->setItem(row, 3, cell(state));
            if (offer.hash == previous) {
                offers_->selectRow(row);
            }
        }
    }
    const FileOffer *selectedOffer() const
    {
        const auto row = offers_->currentRow();
        const auto &offers = files_->offers();
        return row >= 0 && row < offers.size() ? &offers.at(row) : nullptr;
    }
    bool confirmDownload(const QString &name, const QString &sender, qint64 size)
    {
        return confirmAction(this,
                             tr("Download"),
                             tr("Download and import %1 (%2) from %3? It can add objects, data "
                                "feeds, and maps.")
                                 .arg(name, sizeText(size), sender),
                             tr("Download"));
    }
    void downloadOffer()
    {
        const auto *offer = selectedOffer();
        if (!offer) {
            showStatus(tr("Select a file."));
            return;
        }
        const auto serverId = offer->serverId;
        const auto hash = offer->hash;
        if (!confirmDownload(offer->name, offer->senderCallsign, offer->size) || !files_) {
            return;
        }
        showStatus(files_->download(serverId, hash));
    }
    void showServerFiles()
    {
        serverFiles_->setRowCount(static_cast<int>(serverList_.size()));
        for (int row = 0; row < serverList_.size(); ++row) {
            const auto &file = serverList_.at(row);
            serverFiles_->setItem(row, 0, cell(file.name));
            serverFiles_->setItem(row, 1, cell(file.submitter));
            serverFiles_->setItem(row, 2, cell(sizeText(file.size)));
            serverFiles_->setItem(row,
                                  3,
                                  cell(file.time.isValid() ? file.time.toLocalTime().toString(
                                                                 QStringLiteral("yyyy-MM-dd HH:mm"))
                                                           : QString{}));
        }
    }
    const ServerFile *selectedServerFile() const
    {
        const auto row = serverFiles_->currentRow();
        return row >= 0 && row < serverList_.size() ? &serverList_.at(row) : nullptr;
    }
    void downloadServerFile()
    {
        const auto *selected = selectedServerFile();
        if (!selected) {
            showStatus(tr("Select a file."));
            return;
        }
        const auto file = *selected;
        if (!confirmDownload(file.name, file.submitter, file.size) || !files_) {
            return;
        }
        const auto error = files_->downloadServerFile(searchedServer_, file);
        showStatus(error.isEmpty() ? tr("Downloading %1. Received shows the result.").arg(file.name)
                                   : error);
    }
    void deleteServerFile()
    {
        const auto *selected = selectedServerFile();
        if (!selected) {
            showStatus(tr("Select a file."));
            return;
        }
        const auto file = *selected;
        if (!confirmAction(this,
                           tr("Delete"),
                           tr("Delete %1 from the server for all users?").arg(file.name),
                           tr("Delete")) ||
            !files_) {
            return;
        }
        files_->remove(searchedServer_, file.hash);
    }
    void sendPackage()
    {
        Modal<QDialog> dialog(this);
        dialog->setObjectName(QStringLiteral("sendPackageDialog"));
        dialog->setWindowTitle(tr("Send Package"));
        auto *form = new QFormLayout(dialog.get());
        auto *name = new QLineEdit(tr("SWAK package"), dialog.get());
        name->setObjectName(QStringLiteral("packageName"));
        name->setMaxLength(100);
        form->addRow(tr("Name"), name);
        auto *server = new QComboBox(dialog.get());
        server->setObjectName(QStringLiteral("packageServer"));
        for (int i = 0; i < serverChoice_->count(); ++i) {
            server->addItem(serverChoice_->itemText(i), serverChoice_->itemData(i));
        }
        form->addRow(tr("TAK Server"), server);
        auto *objects = new QListWidget(dialog.get());
        objects->setObjectName(QStringLiteral("packageObjects"));
        if (objects_) {
            for (const auto &item : objects_->items()) {
                if (sharedCotType(item).isEmpty()) {
                    continue;
                }
                auto *entry = new QListWidgetItem(item.title, objects);
                entry->setData(Qt::UserRole, item.id);
                entry->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
                entry->setCheckState(Qt::Unchecked);
            }
        }
        form->addRow(tr("Objects"), objects);
        auto *people = new QListWidget(dialog.get());
        people->setObjectName(QStringLiteral("packageRecipients"));
        if (contacts_) {
            for (const auto &contact : contacts_->contacts()) {
                auto *entry = new QListWidgetItem(
                    contact.callsign.isEmpty() ? contact.uid : contact.callsign, people);
                entry->setData(Qt::UserRole, contact.uid);
                entry->setData(Qt::UserRole + 1, contact.callsign);
                entry->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
                entry->setCheckState(Qt::Unchecked);
            }
        }
        form->addRow(tr("Recipients"), people);
        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.get());
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Send"));
        connect(buttons, &QDialogButtonBox::accepted, dialog.get(), &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, dialog.get(), &QDialog::reject);
        form->addRow(buttons);
        if (dialog->exec() != QDialog::Accepted || !dialog || !files_) {
            return;
        }
        QStringList ids;
        for (int row = 0; row < objects->count(); ++row) {
            if (objects->item(row)->checkState() == Qt::Checked) {
                ids.append(objects->item(row)->data(Qt::UserRole).toString());
            }
        }
        QList<ShareRecipient> recipients;
        for (int row = 0; row < people->count(); ++row) {
            const auto *entry = people->item(row);
            if (entry->checkState() == Qt::Checked) {
                recipients.append({entry->data(Qt::UserRole).toString(),
                                   entry->data(Qt::UserRole + 1).toString()});
            }
        }
        const auto error =
            files_->send(server->currentData().toString(), name->text(), ids, recipients);
        showStatus(error.isEmpty() ? tr("Uploading the package…") : error);
    }
    QPointer<FileShares> files_;
    QPointer<Servers> servers_;
    QPointer<Contacts> contacts_;
    QPointer<UserObjects> objects_;
    QTabWidget *tabs_ = nullptr;
    QTableWidget *offers_ = nullptr;
    QComboBox *serverChoice_ = nullptr;
    QTableWidget *serverFiles_ = nullptr;
    SingleLineLabel *status_ = nullptr;
    QString searchedServer_;
    QList<ServerFile> serverList_;
};
} // namespace
class FilesPlugin final : public QObject, public Plugin, public PanePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "files/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::PanePlugin)
public:
    QString name() const override
    {
        return tr("Files");
    }
    QWidget *create(PluginServices &services, QWidget *parent) override
    {
        return new FilesPane(services, parent);
    }
};
} // namespace swak
#include "files.moc"

#include "core/server_browser.h"

#include "core/delete_confirmation.h"
#include "core/maps.h"
#include "core/modal.h"
#include "core/provider_table.h"
#include "core/server_layers.h"
#include "core/servers.h"
#include "core/settings_dialog.h"
#include "core/single_line_label.h"
#include "core/tak_channels.h"
#include "core/tak_enrollment.h"
#include "core/tak_server_api.h"

#include <QCollator>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
namespace swak {
LocalLanStatus::LocalLanStatus(Servers &servers, QWidget *parent)
    : QWidget(parent), servers_(servers)
{
    setObjectName(QStringLiteral("localLanStatus"));
    auto *layout = new QVBoxLayout(this);
    status_ = new QPlainTextEdit(this);
    status_->setObjectName(QStringLiteral("localLanSummary"));
    status_->setAccessibleName(tr("Local LAN status"));
    status_->setReadOnly(true);
    status_->setMaximumHeight(110);
    layout->addWidget(status_);
    peers_ = new QTableWidget(0, 7, this);
    peers_->setObjectName(QStringLiteral("localLanPeers"));
    peers_->setAccessibleName(tr("Local peers"));
    peers_->setHorizontalHeaderLabels({tr("Server"),
                                       tr("Peer"),
                                       tr("Address"),
                                       tr("Port"),
                                       tr("Status"),
                                       tr("Received"),
                                       tr("Rejected")});
    peers_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    peers_->setSelectionBehavior(QAbstractItemView::SelectRows);
    peers_->setSelectionMode(QAbstractItemView::SingleSelection);
    peers_->verticalHeader()->hide();
    peers_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(peers_);
    connect(&servers_, &Servers::changed, this, &LocalLanStatus::refresh);
    connect(&servers_, &Servers::statusChanged, this, &LocalLanStatus::refresh);
    refresh();
}
void LocalLanStatus::refresh()
{
    const auto sources = servers_.sources();
    const auto stats = servers_.statistics();
    QStringList statuses;
    for (auto it = sources.begin(); it != sources.end(); ++it) {
        const auto source = it.value().toObject();
        if (source.value("protocol") == QLatin1String("lan")) {
            statuses.append(source.value("name").toString() + QStringLiteral(": ") +
                            servers_.status(it.key()));
        }
    }
    const QCollator collator;
    std::sort(statuses.begin(), statuses.end(), [&](const auto &a, const auto &b) {
        return collator.compare(a, b) < 0;
    });
    const auto summary = statuses.isEmpty() ? tr("No Local LAN servers. Add a server with the "
                                                 "Local LAN protocol to enable discovery.")
                                            : statuses.join('\n');
    if (status_->toPlainText() != summary) {
        status_->setPlainText(summary);
    }
    if (auto *item = peers_->item(peers_->currentRow(), 0)) {
        selectedPeer_ = item->data(Qt::UserRole).toString();
    }
    QStringList ids;
    for (auto it = stats.begin(); it != stats.end(); ++it) {
        if (!it.value().toObject().value("peerId").toString().isEmpty()) {
            ids.append(it.key());
        }
    }
    const auto name = [&](const QString &id) {
        return sources.value(stats.value(id).toObject().value("serverId").toString())
            .toObject()
            .value("name")
            .toString();
    };
    std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) {
        const int order = collator.compare(name(a), name(b));
        return order == 0 ? collator.compare(a, b) < 0 : order < 0;
    });
    peers_->setUpdatesEnabled(false);
    peers_->setRowCount(static_cast<int>(ids.size()));
    for (int row = 0; row < ids.size(); ++row) {
        const auto id = ids.at(row);
        const auto peer = stats.value(id).toObject();
        const QStringList values{name(id),
                                 peer.value("peerId").toString(),
                                 peer.value("address").toString(),
                                 QLocale().toString(peer.value("port").toInt()),
                                 peer.value("state").toString(),
                                 QLocale().toString(peer.value("received").toDouble(), 'f', 0),
                                 QLocale().toString(peer.value("rejected").toDouble(), 'f', 0)};
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setData(Qt::UserRole, id);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            peers_->setItem(row, column, item);
        }
        if (id == selectedPeer_) {
            peers_->setCurrentCell(row, 0);
        }
    }
    peers_->setUpdatesEnabled(true);
}
QWidget *Servers::createSettingsWidget(QWidget *parent)
{
    return new ServerBrowser(*this, parent);
}
ServerBrowser::ServerBrowser(Servers &servers, QWidget *parent) : QWidget(parent), servers_(servers)
{
    setObjectName(QStringLiteral("serverBrowser"));
    auto *layout = new QVBoxLayout(this);
    auto *notice =
        new SingleLineLabel(tr("Enable multiple connections. TLS validates certificates. TCP and "
                               "LAN peers use plaintext. UDP input listens on a local address, "
                               "and UDP output sends to a server."),
                            this);
    layout->addWidget(notice);
    table_ = new ProviderTable(false, this);
    table_->setObjectName(QStringLiteral("serverList"));
    table_->setAccessibleName(tr("Servers"));
    table_->setHorizontalHeaderLabels({tr("Active"), tr("Server"), tr("Protocol")});
    layout->addWidget(table_);
    auto *row = new QHBoxLayout;
    const auto button = [this, row](const QString &text, const QString &id) {
        auto *b = new QPushButton(text, this);
        b->setObjectName(id);
        row->addWidget(b);
        return b;
    };
    connect(button(tr("Add"), QStringLiteral("addServer")), &QPushButton::clicked, this, [this] {
        edit(true);
    });
    connect(button(tr("Edit"), QStringLiteral("editServer")), &QPushButton::clicked, this, [this] {
        edit(false);
    });
    connect(button(tr("Delete ..."), QStringLiteral("removeServer")),
            &QPushButton::clicked,
            this,
            [this] {
                const auto id = table_->selectedId();
                if (!id.isEmpty() && confirmDeletion(this,
                                                     servers_.sources()
                                                         .value(id)
                                                         .toObject()
                                                         .value(QStringLiteral("name"))
                                                         .toString(id))) {
                    servers_.remove(id);
                }
            });
    connect(button(tr("Reconnect"), QStringLiteral("reconnectServers")),
            &QPushButton::clicked,
            &servers_,
            &Servers::reconnect);
    // TAK Server commands share one button, so the pane keeps its earlier minimum width.
    auto *tak = button(tr("TAK Server"), QStringLiteral("takServerActions"));
    auto *takMenu = new QMenu(tak);
    takMenu->setObjectName(QStringLiteral("takServerMenu"));
    takMenu->setToolTipsVisible(true);
    tak->setMenu(takMenu);
    enroll_ = takMenu->addAction(tr("Enroll ..."));
    enroll_->setObjectName(QStringLiteral("enrollServer"));
    enroll_->setToolTip(tr("Request a client certificate from TAK Server for the selected TLS "
                           "entry."));
    connect(enroll_, &QAction::triggered, this, &ServerBrowser::enroll);
    auto *channels = takMenu->addAction(tr("Channels ..."));
    channels->setObjectName(QStringLiteral("serverChannels"));
    channels->setToolTip(tr("Choose the TAK Server groups that send and receive data for this "
                            "entry."));
    connect(channels, &QAction::triggered, this, &ServerBrowser::loadChannels);
    auto *layers = takMenu->addAction(tr("Map Layers ..."));
    layers->setObjectName(QStringLiteral("serverMapLayers"));
    layers->setToolTip(tr("Add the map layers of TAK Server as map providers."));
    connect(layers, &QAction::triggered, this, &ServerBrowser::loadMapLayers);
    auto *feeds = takMenu->addAction(tr("Data Feeds ..."));
    feeds->setObjectName(QStringLiteral("serverDataFeeds"));
    feeds->setToolTip(
        tr("List the data feeds of TAK Server and the feeds that this user receives."));
    connect(feeds, &QAction::triggered, this, &ServerBrowser::loadDataFeeds);
    layout->addLayout(row);
    enrollStatus_ = new SingleLineLabel(QString{}, this);
    enrollStatus_->setObjectName(QStringLiteral("enrollStatus"));
    // The line appears only with a message, so the pane keeps its earlier minimum size.
    enrollStatus_->hide();
    layout->addWidget(enrollStatus_);
    status_ = new QPlainTextEdit(this);
    status_->setObjectName(QStringLiteral("serverStatus"));
    status_->setAccessibleName(tr("Connection details"));
    status_->setReadOnly(true);
    status_->setMaximumHeight(110);
    layout->addWidget(status_);
    connect(table_, &QTableWidget::currentCellChanged, this, [this] {
        status();
    });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int col) {
        if (col != 0) {
            edit(false);
        }
    });
    connect(&servers_, &Servers::changed, this, &ServerBrowser::refresh);
    connect(&servers_, &Servers::statusChanged, this, &ServerBrowser::status);
    refresh();
}
void ServerBrowser::refresh()
{
    const auto selected = table_->selectedId();
    const QSignalBlocker blocker(table_);
    table_->clearProviders();
    const auto sources = servers_.sources();
    auto ids = sources.keys();
    const QCollator collator;
    std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) {
        return collator.compare(
                   sources.value(a).toObject().value(QStringLiteral("name")).toString(),
                   sources.value(b).toObject().value(QStringLiteral("name")).toString()) < 0;
    });
    for (const auto &id : ids) {
        const auto source = sources.value(id).toObject();
        auto *active = table_->append(
            id,
            source.value(QStringLiteral("name")).toString(),
            source.value(QStringLiteral("protocol")).toString() == QLatin1String("tls") ? tr("TLS")
            : source.value(QStringLiteral("protocol")).toString() == QLatin1String("stcp")
                ? tr("TCP")
            : source.value(QStringLiteral("protocol")).toString() == QLatin1String("quic")
                ? tr("QUIC")
            : source.value(QStringLiteral("protocol")).toString() == QLatin1String("lan")
                ? tr("Local LAN")
            : source.value(QStringLiteral("protocol")).toString() == QLatin1String("udp")
                ? tr("UDP")
            : source.value(QStringLiteral("protocol")).toString() == QLatin1String("udpout")
                ? tr("UDP output")
            : source.value(QStringLiteral("protocol")).toString() == QLatin1String("mcast")
                ? tr("Multicast mesh")
                : tr("Unavailable"),
            source.value("protocol") == QLatin1String("lan")
                ? source.value("localAddress").toString()
                : source.value(QStringLiteral("host")).toString() + QStringLiteral(":") +
                      QString::number(source.value(QStringLiteral("port")).toInt()),
            source.value(QStringLiteral("enabled")).toBool(),
            validateServer(source).isEmpty());
        connect(active, &QAbstractButton::clicked, this, [this, id](bool enabled) {
            QTimer::singleShot(0, this, [this, id, enabled] {
                auto currentSource = servers_.sources().value(id).toObject();
                if (currentSource.isEmpty()) {
                    return;
                }
                currentSource.insert(QStringLiteral("enabled"), enabled);
                const auto error = servers_.put(id, currentSource);
                if (!error.isEmpty()) {
                    status_->setPlainText(error);
                }
            });
        });
    }
    table_->selectId(selected);
    if (table_->currentRow() < 0 && table_->rowCount()) {
        table_->setCurrentCell(0, 1);
    }
    status();
}
void ServerBrowser::setMapSources(MapSources *maps)
{
    maps_ = maps;
}
void ServerBrowser::loadMapLayers()
{
    const auto id = table_->selectedId();
    auto *api = servers_.findChild<TakServerApi *>();
    if (!maps_) {
        showEnrollStatus(tr("This window has no map catalog."));
        return;
    }
    if (!api || TakServerApi::apiUrl(servers_.sources().value(id).toObject(), QStringLiteral("/"))
                    .isEmpty()) {
        showEnrollStatus(tr("Select a TLS or QUIC entry with a TAK REST API port first."));
        return;
    }
    showEnrollStatus(tr("Reading the map layers of the server…"));
    api->request(
        id,
        "GET",
        QStringLiteral("/maplayers/all"),
        {},
        this,
        [this, id](const NetworkReply &reply) {
            if (!reply.ok()) {
                showEnrollStatus(reply.error.isEmpty()
                                     ? tr("The server answered with HTTP %1.").arg(reply.status)
                                     : reply.error);
                return;
            }
            chooseMapLayers(id, reply.body);
        },
        2 * 1024 * 1024);
}
void ServerBrowser::chooseMapLayers(const QString &id, const QByteArray &json)
{
    QList<ServerMapLayer> layers;
    QStringList skipped;
    const auto serverName =
        servers_.sources().value(id).toObject().value(QStringLiteral("name")).toString();
    if (!readServerMapLayers(json, id, serverName, &layers, &skipped)) {
        showEnrollStatus(tr("The map layer list is invalid."));
        return;
    }
    if (layers.isEmpty()) {
        showEnrollStatus(skipped.isEmpty() ? tr("The server has no map layers.") : skipped.first());
        return;
    }
    showEnrollStatus({});
    Modal<QDialog> dialog(this);
    dialog->setObjectName(QStringLiteral("mapLayerDialog"));
    dialog->setWindowTitle(tr("Map Layers"));
    auto *layout = new QVBoxLayout(dialog.get());
    auto *notice = new QLabel(tr("Checked layers become map providers. Select one in Map "
                                 "Providers to show it."),
                              dialog.get());
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *list = new QListWidget(dialog.get());
    list->setObjectName(QStringLiteral("mapLayerList"));
    for (const auto &layer : std::as_const(layers)) {
        auto *item = new QListWidgetItem(layer.name, list);
        item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        item->setCheckState(maps_ && maps_->sources().contains(layer.id) ? Qt::Checked
                                                                         : Qt::Unchecked);
        item->setToolTip(layer.source.value(QStringLiteral("endpoint")).toString());
    }
    layout->addWidget(list);
    if (!skipped.isEmpty()) {
        auto *other = new QLabel(skipped.join(QLatin1Char('\n')), dialog.get());
        other->setObjectName(QStringLiteral("skippedMapLayers"));
        other->setWordWrap(true);
        layout->addWidget(other);
    }
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.get());
    connect(buttons, &QDialogButtonBox::accepted, dialog.get(), &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog.get(), &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog->exec() != QDialog::Accepted || !dialog || !maps_) {
        return;
    }
    int added = 0;
    QStringList errors;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->checkState() != Qt::Checked) {
            continue;
        }
        const auto &layer = layers.at(row);
        if (const auto error = maps_->put(layer.id, layer.source); !error.isEmpty()) {
            errors.append(QStringLiteral("%1: %2").arg(layer.name, error));
        } else {
            ++added;
        }
    }
    showEnrollStatus(
        errors.isEmpty()
            ? tr("%n map providers added. Choose one in Map Providers.", nullptr, added)
            : errors.first());
}
void ServerBrowser::loadDataFeeds()
{
    const auto id = table_->selectedId();
    auto *api = servers_.findChild<TakServerApi *>();
    if (!api || TakServerApi::apiUrl(servers_.sources().value(id).toObject(), QStringLiteral("/"))
                    .isEmpty()) {
        showEnrollStatus(tr("Select a TLS or QUIC entry with a TAK REST API port first."));
        return;
    }
    showEnrollStatus(tr("Reading the data feeds of the server…"));
    struct State
    {
        int pending = 2;
        QByteArray feeds;
        QByteArray groups;
        QString error;
    };
    auto state = std::make_shared<State>();
    const auto finish = [this, state] {
        if (--state->pending > 0) {
            return;
        }
        QList<ServerFeed> list;
        if (!state->error.isEmpty()) {
            showEnrollStatus(state->error);
            return;
        }
        if (!readServerFeeds(state->feeds, &list)) {
            showEnrollStatus(tr("The data feed list is invalid."));
            return;
        }
        QList<TakGroup> groups;
        // Without the group list, SWAK cannot tell which feeds the user receives.
        const bool known = readTakGroups(state->groups, &groups);
        showDataFeeds(list, known ? activeChannels(groups) : QSet<QString>{}, known);
    };
    api->request(
        id,
        "GET",
        QStringLiteral("/datafeeds"),
        {},
        this,
        [state, finish](const NetworkReply &reply) {
            if (reply.ok()) {
                state->feeds = reply.body;
            } else {
                state->error = reply.error.isEmpty()
                                   ? tr("The server answered with HTTP %1.").arg(reply.status)
                                   : reply.error;
            }
            finish();
        },
        2 * 1024 * 1024);
    api->request(
        id,
        "GET",
        QStringLiteral("/groups/all?useCache=true"),
        {},
        this,
        [state, finish](const NetworkReply &reply) {
            if (reply.ok()) {
                state->groups = reply.body;
            }
            finish();
        },
        1024 * 1024);
}
void ServerBrowser::showDataFeeds(const QList<ServerFeed> &feeds,
                                  const QSet<QString> &channels,
                                  bool channelsKnown)
{
    if (feeds.isEmpty()) {
        showEnrollStatus(tr("The server has no data feeds."));
        return;
    }
    showEnrollStatus({});
    Modal<QDialog> dialog(this);
    dialog->setObjectName(QStringLiteral("dataFeedDialog"));
    dialog->setWindowTitle(tr("Data Feeds"));
    auto *layout = new QVBoxLayout(dialog.get());
    auto *table = new QTableWidget(static_cast<int>(feeds.size()), 5, dialog.get());
    table->setObjectName(QStringLiteral("dataFeedTable"));
    table->setHorizontalHeaderLabels(
        {tr("Name"), tr("Type"), tr("Input"), tr("Groups"), tr("You receive")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setStretchLastSection(true);
    for (int row = 0; row < feeds.size(); ++row) {
        const auto &feed = feeds.at(row);
        const auto input = feed.port > 0 ? QStringLiteral("%1 %2").arg(feed.protocol).arg(feed.port)
                                         : feed.protocol;
        const auto received = !feed.active                   ? tr("No, inactive")
                              : !channelsKnown               ? tr("Unknown")
                              : feedReceived(feed, channels) ? tr("Yes")
                                                             : tr("No");
        const QStringList cells{feed.name,
                                feed.type,
                                input,
                                feed.groups.isEmpty() ? tr("All users")
                                                      : feed.groups.join(QStringLiteral(", ")),
                                received};
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            item->setToolTip(column == 0 && !feed.tags.isEmpty()
                                 ? feed.tags.join(QStringLiteral(", "))
                                 : cells.at(column));
            table->setItem(row, column, item);
        }
    }
    layout->addWidget(table);
    auto *note = new QLabel(tr("Choose channels in TAK Server > Channels ... to change the feeds "
                               "that you receive."),
                            dialog.get());
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog.get());
    connect(buttons, &QDialogButtonBox::rejected, dialog.get(), &QDialog::reject);
    layout->addWidget(buttons);
    dialog->resize(640, 360);
    dialog->exec();
}
void ServerBrowser::loadChannels()
{
    const auto id = table_->selectedId();
    auto *api = servers_.findChild<TakServerApi *>();
    if (!api || TakServerApi::apiUrl(servers_.sources().value(id).toObject(), QStringLiteral("/"))
                    .isEmpty()) {
        showEnrollStatus(tr("Select a TLS or QUIC entry with a TAK REST API port first."));
        return;
    }
    if (!channels_) {
        channels_ = new TakChannels(*api, this);
        connect(channels_, &TakChannels::loaded, this, &ServerBrowser::chooseChannels);
        connect(
            channels_, &TakChannels::applied, this, [this](const QString &, const QString &error) {
                showEnrollStatus(error.isEmpty() ? tr("The active channels are updated.") : error);
            });
    }
    showEnrollStatus(tr("Reading the channels of the server…"));
    channels_->load(id);
}
void ServerBrowser::chooseChannels(const QString &id,
                                   const QList<TakGroup> &groups,
                                   const QString &error)
{
    if (!error.isEmpty()) {
        showEnrollStatus(error);
        return;
    }
    const auto names = channelNames(groups);
    if (names.isEmpty()) {
        showEnrollStatus(tr("The server has no channels for this user."));
        return;
    }
    showEnrollStatus({});
    Modal<QDialog> dialog(this);
    dialog->setObjectName(QStringLiteral("channelDialog"));
    dialog->setWindowTitle(tr("Channels"));
    auto *layout = new QVBoxLayout(dialog.get());
    auto *notice = new QLabel(tr("Checked channels send and receive data for this user. TAK Server "
                                 "applies the choice to every client of the user."),
                              dialog.get());
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *list = new QListWidget(dialog.get());
    list->setObjectName(QStringLiteral("channelList"));
    const auto active = activeChannels(groups);
    for (const auto &name : names) {
        auto *item = new QListWidgetItem(name, list);
        item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        item->setCheckState(active.contains(name) ? Qt::Checked : Qt::Unchecked);
        for (const auto &group : groups) {
            if (group.name == name && !group.description.isEmpty()) {
                item->setToolTip(group.description);
            }
        }
    }
    layout->addWidget(list);
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.get());
    connect(buttons, &QDialogButtonBox::accepted, dialog.get(), &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog.get(), &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog->exec() != QDialog::Accepted || !dialog) {
        return;
    }
    QSet<QString> chosen;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->checkState() == Qt::Checked) {
            chosen.insert(list->item(row)->text());
        }
    }
    showEnrollStatus(tr("Updating the active channels…"));
    channels_->apply(id, groups, chosen, servers_.senderUid());
}
void ServerBrowser::showEnrollStatus(const QString &text)
{
    enrollStatus_->setText(text);
    enrollStatus_->setVisible(!text.isEmpty());
}
void ServerBrowser::enroll()
{
    const auto id = table_->selectedId();
    auto *api = servers_.findChild<TakServerApi *>();
    const auto entry = servers_.sources().value(id).toObject();
    if (!api || entry.value(QStringLiteral("protocol")).toString() != u"tls") {
        showEnrollStatus(tr("Select a TLS server entry first."));
        return;
    }
    Modal<QDialog> dialog(this);
    dialog->setObjectName(QStringLiteral("enrollDialog"));
    dialog->setWindowTitle(tr("Enroll for a client certificate"));
    auto *layout = new QVBoxLayout(dialog.get());
    auto *notice = new QLabel(
        tr("SWAK asks TAK Server to sign a client certificate for this entry. The password is "
           "used once and is not saved. The key and certificates are saved in a private folder. "
           "The entry's password variable, when set, encrypts the key."),
        dialog.get());
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *form = new QFormLayout;
    auto *user = new QLineEdit(entry.value(QStringLiteral("authUser")).toString(), dialog.get());
    user->setObjectName(QStringLiteral("enrollUser"));
    user->setMaxLength(128);
    auto *password = new QLineEdit(dialog.get());
    password->setObjectName(QStringLiteral("enrollPassword"));
    password->setEchoMode(QLineEdit::Password);
    password->setMaxLength(1024);
    auto *port = new QSpinBox(dialog.get());
    port->setObjectName(QStringLiteral("enrollPort"));
    port->setRange(1, 65535);
    port->setValue(8446);
    auto *keyChoice = new QComboBox(dialog.get());
    keyChoice->setObjectName(QStringLiteral("enrollKeyChoice"));
    keyChoice->addItems({tr("Create a new key"), tr("Use an existing PEM key file")});
    auto *keyFile = new QLineEdit(dialog.get());
    keyFile->setObjectName(QStringLiteral("enrollKeyFile"));
    keyFile->setPlaceholderText(tr("Path to the PEM private key"));
    keyFile->setEnabled(false);
    connect(keyChoice, &QComboBox::currentIndexChanged, keyFile, [keyFile](int index) {
        keyFile->setEnabled(index == 1);
    });
    form->addRow(tr("Username"), user);
    form->addRow(tr("Password"), password);
    form->addRow(tr("Enrollment port"), port);
    form->addRow(tr("Private key"), keyChoice);
    form->addRow(tr("Key file"), keyFile);
    layout->addLayout(form);
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog.get());
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Enroll"));
    connect(buttons, &QDialogButtonBox::accepted, dialog.get(), &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog.get(), &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog->exec() != QDialog::Accepted || !dialog) {
        if (dialog) {
            password->clear();
        }
        return;
    }
    if (!enrollment_) {
        enrollment_ = new TakEnrollment(servers_, api->network(), this);
        connect(enrollment_, &TakEnrollment::progress, this, &ServerBrowser::showEnrollStatus);
        connect(enrollment_, &TakEnrollment::finished, this, [this](const QString &error) {
            showEnrollStatus(error.isEmpty()
                                 ? tr("Enrollment complete. The entry uses the new client "
                                      "certificate.")
                                 : error);
            enroll_->setEnabled(true);
        });
    }
    TakEnrollment::Options options;
    options.serverId = id;
    options.username = user->text().trimmed();
    options.password = password->text().toUtf8();
    password->clear();
    options.port = port->value();
    options.keyFile = keyChoice->currentIndex() == 1 ? keyFile->text().trimmed() : QString{};
    options.clientUid = servers_.senderUid();
    const auto error = enrollment_->start(std::move(options));
    showEnrollStatus(error);
    enroll_->setEnabled(!enrollment_->running());
}
void ServerBrowser::status()
{
    const auto id = table_->selectedId();
    const auto text = id.isEmpty() ? QString{} : servers_.status(id);
    if (status_->toPlainText() != text) {
        status_->setPlainText(text);
    }
}
void ServerBrowser::edit(bool adding)
{
    if (adding && servers_.sources().size() >= 16) {
        status_->setPlainText(tr("The server limit is 16."));
        return;
    }
    if (!adding && table_->selectedId().isEmpty()) {
        return;
    }
    const auto id =
        adding ? QUuid::createUuid().toString(QUuid::WithoutBraces) : table_->selectedId();
    QString error;
    Modal<SettingsDialog> dialog(
        QList<SettingsPage>{
            {id,
             tr("CoT server"),
             serverFields(adding ? serverDefaults() : servers_.sources().value(id).toObject()),
             adding ? serverDefaults() : servers_.sources().value(id).toObject(),
             validateServer,
             [this, id, &error](const QJsonObject &v) {
                 error = servers_.put(id, v);
             }}},
        [&error] {
            return error;
        },
        this,
        true,
        adding);
    dialog->setObjectName(QStringLiteral("serverEditor"));
    dialog->setWindowTitle(adding ? tr("Add server") : tr("Edit server"));
    if (dialog->exec() == QDialog::Accepted) {
        table_->selectId(id);
    }
}
} // namespace swak

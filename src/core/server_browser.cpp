#include "core/server_browser.h"

#include "core/delete_confirmation.h"
#include "core/modal.h"
#include "core/provider_table.h"
#include "core/servers.h"
#include "core/settings_dialog.h"
#include "core/single_line_label.h"

#include <QCollator>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
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
                               "LAN peers use plaintext. UDP listens on a local address."),
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
    layout->addLayout(row);
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
            : source.value(QStringLiteral("protocol")).toString() == QLatin1String("lan")
                ? tr("Local LAN")
            : source.value(QStringLiteral("protocol")).toString() == QLatin1String("udp")
                ? tr("UDP")
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

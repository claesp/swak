#include "core/server_browser.h"
#include "core/servers.h"
#include "core/provider_table.h"
#include "core/settings_dialog.h"
#include "core/single_line_label.h"
#include <QPushButton>
#include <QVBoxLayout>
#include <QCollator>
#include <QSignalBlocker>
#include <QNetworkInterface>
#include <QUuid>
#include <algorithm>
namespace swak {
QWidget *Servers::createSettingsWidget(QWidget *parent) { return new ServerBrowser(*this, parent); }
ServerBrowser::ServerBrowser(Servers &servers, QWidget *parent) : QWidget(parent), servers_(servers)
{
    setObjectName(QStringLiteral("serverBrowser"));
    auto *layout = new QVBoxLayout(this);
    auto *notice = new SingleLineLabel(tr("Enable multiple connections. TLS validates certificates. TCP and LAN peers use plaintext. UDP listens on a local address."), this);
    layout->addWidget(notice);
    table_ = new ProviderTable(false, this); table_->setObjectName(QStringLiteral("serverList")); table_->setAccessibleName(tr("Servers"));
    table_->setHorizontalHeaderLabels({tr("Active"), tr("Server"), tr("Protocol")}); layout->addWidget(table_);
    auto *row = new QHBoxLayout;
    const auto button = [this, row](const QString &text, const QString &id) { auto *b = new QPushButton(text, this); b->setObjectName(id); row->addWidget(b); return b; };
    connect(button(tr("Add"), QStringLiteral("addServer")), &QPushButton::clicked, this, [this] { edit(true); });
    connect(button(tr("Edit"), QStringLiteral("editServer")), &QPushButton::clicked, this, [this] { edit(false); });
    connect(button(tr("Delete"), QStringLiteral("removeServer")), &QPushButton::clicked, this, [this] { servers_.remove(table_->selectedId()); });
    connect(button(tr("Reconnect"), QStringLiteral("reconnectServers")), &QPushButton::clicked, &servers_, &Servers::reconnect);
    connect(button(tr("LAN peers"), QStringLiteral("lanPeers")), &QPushButton::clicked, this, [this] {
        QStringList labels; QJsonArray addresses;
        const auto current = servers_.lan().value(QStringLiteral("address")).toString();
        for (const auto &interface : QNetworkInterface::allInterfaces()) for (const auto &entry : interface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback() && interface.flags().testFlag(QNetworkInterface::IsUp)) { labels.append(interface.humanReadableName() + QStringLiteral(" / ") + entry.ip().toString()); addresses.append(entry.ip().toString()); }
        if (!addresses.contains(current)) { labels.append(current); addresses.append(current); }
        QString error;
        SettingsDialog dialog({{QStringLiteral("lan"), tr("LAN peers"), {booleanSetting(QStringLiteral("enabled"), tr("Enable plaintext LAN discovery and CoT exchange")),
            choiceSetting(QStringLiteral("address"), tr("Local network"), labels, addresses)}, servers_.lan(), {}, [this, &error](const QJsonObject &v) { error = servers_.setLan(v); }}}, [&error] { return error; }, this, true);
        dialog.setWindowTitle(tr("LAN peers")); dialog.exec();
    });
    layout->addLayout(row);
    status_ = new SingleLineLabel(this); status_->setObjectName(QStringLiteral("serverStatus")); status_->setTextFormat(Qt::PlainText); layout->addWidget(status_);
    connect(table_, &QTableWidget::currentCellChanged, this, [this] { status(); });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int col) { if (col != 0) edit(false); });
    connect(&servers_, &Servers::changed, this, &ServerBrowser::refresh);
    connect(&servers_, &Servers::statusChanged, this, &ServerBrowser::status);
    refresh();
}
void ServerBrowser::refresh()
{
    const auto selected = table_->selectedId(); const QSignalBlocker blocker(table_); table_->clearProviders();
    const auto sources = servers_.sources(); auto ids = sources.keys(); const QCollator collator;
    std::sort(ids.begin(), ids.end(), [&](const auto &a, const auto &b) { return collator.compare(sources.value(a).toObject().value(QStringLiteral("name")).toString(), sources.value(b).toObject().value(QStringLiteral("name")).toString()) < 0; });
    for (const auto &id : ids) {
        const auto source = sources.value(id).toObject();
        auto *active = table_->append(id, source.value(QStringLiteral("name")).toString(), source.value(QStringLiteral("protocol")).toString() == QLatin1String("tls") ? tr("TLS") : source.value(QStringLiteral("protocol")).toString() == QLatin1String("stcp") ? tr("TCP") : source.value(QStringLiteral("protocol")).toString() == QLatin1String("udp") ? tr("UDP") : tr("Unavailable"),
            source.value(QStringLiteral("host")).toString() + QStringLiteral(":") + QString::number(source.value(QStringLiteral("port")).toInt()), source.value(QStringLiteral("enabled")).toBool(), validateServer(source).isEmpty());
        connect(active, &QAbstractButton::clicked, this, [this, id](bool enabled) { QTimer::singleShot(0, this, [this, id, enabled] { auto source = servers_.sources().value(id).toObject(); if (source.isEmpty()) return; source.insert(QStringLiteral("enabled"), enabled); const auto error = servers_.put(id, source); if (!error.isEmpty()) status_->setText(error); }); });
    }
    table_->selectId(selected); if (table_->currentRow() < 0 && table_->rowCount()) table_->setCurrentCell(0, 1); status();
}
void ServerBrowser::status()
{
    QStringList peers;
    const auto stats = servers_.statistics();
    for (auto it = stats.begin(); it != stats.end(); ++it) if (it.key().startsWith(QLatin1String("LAN/"))) peers.append(it.key().mid(4) + QStringLiteral(": ") + it.value().toObject().value(QStringLiteral("state")).toString());
    status_->setText((table_->selectedId().isEmpty() ? QString{} : servers_.status(table_->selectedId()) + QStringLiteral(" ")) + stats.value(QStringLiteral("lan")).toString(tr("LAN discovery inactive")) + tr(". Peers: %1.").arg(peers.size()));
    status_->setToolTip(peers.join(QLatin1Char('\n')).toHtmlEscaped());
}
void ServerBrowser::edit(bool adding)
{
    if (adding && servers_.sources().size() >= 16) { status_->setText(tr("The server limit is 16.")); return; }
    if (!adding && table_->selectedId().isEmpty()) return;
    const auto id = adding ? QUuid::createUuid().toString(QUuid::WithoutBraces) : table_->selectedId(); QString error;
    SettingsDialog dialog({{id, tr("CoT server"), serverFields(), adding ? serverDefaults() : servers_.sources().value(id).toObject(), validateServer,
        [this, id, &error](const QJsonObject &v) { error = servers_.put(id, v); }}}, [&error] { return error; }, this, true, adding);
    dialog.setObjectName(QStringLiteral("serverEditor")); dialog.setWindowTitle(adding ? tr("Add server") : tr("Edit server"));
    if (dialog.exec() == QDialog::Accepted) table_->selectId(id);
}
}

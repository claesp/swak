#include "core/lan_protocol.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <optional>
namespace swak {
QByteArray lanAnnouncement(const QString &id, quint16 port)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("protocol"), QStringLiteral("SWAK-COT")},
                                     {QStringLiteral("version"), 1},
                                     {QStringLiteral("id"), id},
                                     {QStringLiteral("port"), port}})
        .toJson(QJsonDocument::Compact);
}
std::optional<LanPeer> parseLanAnnouncement(const QByteArray &bytes,
                                            const QHostAddress &sender,
                                            const QHostAddress &local,
                                            int prefix)
{
    if (bytes.isEmpty() || bytes.size() > 512 || prefix < 1 || prefix > 30 ||
        sender.protocol() != QAbstractSocket::IPv4Protocol ||
        local.protocol() != QAbstractSocket::IPv4Protocol || sender == local ||
        sender.isMulticast() || sender.isLoopback() || !sender.isInSubnet(local, prefix)) {
        return {};
    }
    const quint32 mask = 0xffffffffU << (32 - prefix);
    const auto host = sender.toIPv4Address() & ~mask;
    if (host == 0 || host == ~mask) {
        return {};
    }
    const auto value = QJsonDocument::fromJson(bytes).object();
    const auto id = value.value(QStringLiteral("id")).toString();
    const auto port = value.value(QStringLiteral("port"));
    if (value.value(QStringLiteral("protocol")) != QLatin1String("SWAK-COT") ||
        value.value(QStringLiteral("version")).toDouble() != 1 || QUuid(id).isNull() ||
        id != QUuid(id).toString(QUuid::WithoutBraces) || !port.isDouble() ||
        port.toDouble() != port.toInt() || port.toInt() < 1 || port.toInt() > 65535) {
        return {};
    }
    return LanPeer{id, sender, static_cast<quint16>(port.toInt())};
}
} // namespace swak

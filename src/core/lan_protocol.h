#pragma once
#include <QByteArray>
#include <QHostAddress>
#include <QString>
#include <optional>
namespace swak {
struct LanPeer { QString id; QHostAddress address; quint16 port; };
QByteArray lanAnnouncement(const QString &id, quint16 port);
std::optional<LanPeer> parseLanAnnouncement(const QByteArray &bytes, const QHostAddress &sender, const QHostAddress &local, int prefix);
}

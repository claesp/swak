#pragma once
#include <QByteArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace swak {
// One data feed from the TAK Server data feed catalog (/Marti/api/datafeeds).
struct ServerFeed
{
    QString uuid;
    QString name;
    QString type;
    QString protocol;
    int port = 0;
    bool active = false;
    // The groups that receive the feed. An empty list means all users.
    QStringList groups;
    QStringList tags;
};
inline constexpr int MaxServerFeeds = 512;

// Reads a /Marti/api/datafeeds reply. Returns false for other content.
bool readServerFeeds(const QByteArray &json, QList<ServerFeed> *feeds);
// True when this user receives the feed: the feed is active, and it has no groups or a group
// that is an active channel of the user.
bool feedReceived(const ServerFeed &feed, const QSet<QString> &activeChannels);
} // namespace swak

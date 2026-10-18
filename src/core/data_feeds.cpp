#include "core/data_feeds.h"

#include "core/text.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace swak {
namespace {
QString plain(const QString &text, qsizetype limit = 128)
{
    return plainText(text, limit);
}
QStringList strings(const QJsonValue &value, int limit)
{
    QStringList result;
    for (const auto &entry : value.toArray()) {
        const auto text = plain(entry.toString(), 64);
        if (!text.isEmpty() && result.size() < limit) {
            result.append(text);
        }
    }
    return result;
}
} // namespace

bool readServerFeeds(const QByteArray &json, QList<ServerFeed> *feeds)
{
    const auto document = QJsonDocument::fromJson(json);
    const auto data = document.object().value(QStringLiteral("data"));
    if (!document.isObject() || !data.isArray() || data.toArray().size() > MaxServerFeeds) {
        return false;
    }
    QList<ServerFeed> result;
    for (const auto &value : data.toArray()) {
        const auto object = value.toObject();
        ServerFeed feed;
        feed.uuid = plain(object.value(QStringLiteral("uuid")).toString());
        feed.name = plain(object.value(QStringLiteral("name")).toString());
        if (feed.name.isEmpty()) {
            feed.name = feed.uuid;
        }
        if (feed.name.isEmpty()) {
            continue;
        }
        feed.type = plain(object.value(QStringLiteral("type")).toString(), 32);
        feed.protocol = plain(object.value(QStringLiteral("protocol")).toString(), 32);
        feed.port = std::clamp(object.value(QStringLiteral("port")).toInt(), 0, 65535);
        feed.active = object.value(QStringLiteral("active")).toBool(true);
        feed.groups = strings(object.value(QStringLiteral("filterGroups")), 64);
        if (feed.groups.isEmpty()) {
            const auto group = plain(object.value(QStringLiteral("feedGroup")).toString(), 64);
            if (!group.isEmpty()) {
                feed.groups.append(group);
            }
        }
        feed.tags = strings(object.value(QStringLiteral("tags")), 16);
        result.append(feed);
    }
    *feeds = result;
    return true;
}
bool feedReceived(const ServerFeed &feed, const QSet<QString> &activeChannels)
{
    if (!feed.active) {
        return false;
    }
    return feed.groups.isEmpty() || std::any_of(feed.groups.cbegin(),
                                                feed.groups.cend(),
                                                [&activeChannels](const QString &group) {
                                                    return activeChannels.contains(group);
                                                });
}
} // namespace swak

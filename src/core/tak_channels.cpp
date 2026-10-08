#include "core/tak_channels.h"

#include "core/tak_server_api.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrlQuery>

#include <algorithm>

namespace swak {
namespace {
constexpr int MaxGroups = 512;
QString tr(const char *text)
{
    return QCoreApplication::translate("TakChannels", text);
}
bool safeName(const QString &name)
{
    return !name.isEmpty() && name.size() <= 256 &&
           std::none_of(name.cbegin(), name.cend(), [](QChar ch) {
               return ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format;
           });
}
QString replyError(const NetworkReply &reply)
{
    if (!reply.error.isEmpty()) {
        return reply.error;
    }
    if (reply.status == 401 || reply.status == 403) {
        return tr("The server does not allow channel changes for this user.");
    }
    return tr("The server answered with HTTP status %1.").arg(reply.status);
}
} // namespace
bool readTakGroups(const QByteArray &json, QList<TakGroup> *groups)
{
    const auto data = QJsonDocument::fromJson(json).object().value(QStringLiteral("data"));
    if (!data.isArray() || data.toArray().size() > MaxGroups) {
        return false;
    }
    QList<TakGroup> result;
    for (const auto &value : data.toArray()) {
        const auto object = value.toObject();
        TakGroup group{object.value(QStringLiteral("name")).toString(),
                       object.value(QStringLiteral("direction")).toString(),
                       object.value(QStringLiteral("description")).toString().left(512),
                       object.value(QStringLiteral("active")).toBool(),
                       object};
        if (!safeName(group.name)) {
            return false;
        }
        result.append(group);
    }
    *groups = result;
    return true;
}
QByteArray activeGroupsBody(const QList<TakGroup> &groups, const QSet<QString> &activeNames)
{
    QJsonArray body;
    for (const auto &group : groups) {
        auto object = group.raw;
        object.insert(QStringLiteral("active"), activeNames.contains(group.name));
        body.append(object);
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}
QStringList channelNames(const QList<TakGroup> &groups)
{
    QStringList names;
    for (const auto &group : groups) {
        if (!names.contains(group.name)) {
            names.append(group.name);
        }
    }
    return names;
}
QSet<QString> activeChannels(const QList<TakGroup> &groups)
{
    QSet<QString> names;
    for (const auto &group : groups) {
        if (group.active) {
            names.insert(group.name);
        }
    }
    return names;
}
TakChannels::TakChannels(TakServerApi &api, QObject *parent) : QObject(parent), api_(api) {}
void TakChannels::load(const QString &serverId)
{
    api_.request(serverId,
                 "GET",
                 QStringLiteral("/groups/all?useCache=true"),
                 {},
                 this,
                 [this, serverId](const NetworkReply &reply) {
                     QList<TakGroup> groups;
                     if (!reply.ok()) {
                         emit loaded(serverId, {}, replyError(reply));
                     } else if (!readTakGroups(reply.body, &groups)) {
                         emit loaded(serverId, {}, tr("The server sent an invalid channel list."));
                     } else {
                         emit loaded(serverId, groups, {});
                     }
                 });
}
void TakChannels::apply(const QString &serverId,
                        const QList<TakGroup> &groups,
                        const QSet<QString> &activeNames,
                        const QString &clientUid)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("clientUid"), clientUid.left(128));
    api_.request(serverId,
                 "PUT",
                 QStringLiteral("/groups/active?") + query.toString(QUrl::FullyEncoded),
                 activeGroupsBody(groups, activeNames),
                 this,
                 [this, serverId](const NetworkReply &reply) {
                     emit applied(serverId, reply.ok() ? QString{} : replyError(reply));
                 });
}
} // namespace swak

#include "core/missions.h"

#include "core/cot.h"
#include "core/cot_detail.h"
#include "core/identity.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/tak_server_api.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace swak {
namespace {
constexpr qsizetype MaxListBytes = 8 * 1024 * 1024;
constexpr qsizetype MaxCotBytes = 16 * 1024 * 1024;
constexpr qsizetype MaxArchiveBytes = 64 * 1024 * 1024;
constexpr int RequestTimeoutMs = 60000;
constexpr int MissionLifetimeSeconds = 24 * 60 * 60;

QString plain(const QString &text, qsizetype limit = 128)
{
    QString result;
    for (const auto ch : text.left(limit)) {
        if (ch.category() != QChar::Other_Control && ch.category() != QChar::Other_Format &&
            !ch.isSurrogate()) {
            result += ch;
        }
    }
    return result.trimmed();
}
QDateTime readTime(const QJsonValue &value)
{
    if (value.isDouble()) {
        // Reject values that do not fit in milliseconds before the conversion. A larger value,
        // such as 1e300 from a server, would make the cast undefined.
        const double milliseconds = value.toDouble();
        if (!std::isfinite(milliseconds) || std::abs(milliseconds) > 1e15) {
            return {};
        }
        return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(milliseconds), QTimeZone::UTC);
    }
    return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}
QString segment(const QString &name)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(name));
}
QString query(const QString &value)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}
QString httpError(const NetworkReply &reply)
{
    if (!reply.error.isEmpty()) {
        return reply.error;
    }
    if (reply.status == 401 || reply.status == 403) {
        return Missions::tr(
            "The server does not allow this for your user, or the password is wrong.");
    }
    if (reply.status == 404) {
        return Missions::tr("The server does not have this mission.");
    }
    return Missions::tr("The server answered with HTTP %1.").arg(reply.status);
}
MissionSubscription::Role readRole(const QJsonValue &value)
{
    const auto type = value.toObject().value(QStringLiteral("type")).toString();
    if (type == u"MISSION_OWNER") {
        return MissionSubscription::Role::Owner;
    }
    if (type == u"MISSION_READONLY_SUBSCRIBER") {
        return MissionSubscription::Role::ReadOnly;
    }
    return MissionSubscription::Role::Subscriber;
}
} // namespace

bool validMissionName(const QString &name)
{
    if (name.isEmpty() || name.size() > 128 || name == u"." || name == u".." ||
        plain(name) != name) {
        return false;
    }
    return std::none_of(name.cbegin(), name.cend(), [](QChar ch) {
        return ch == u'/' || ch == u'\\' || ch == u'?' || ch == u'#' || ch == u'%';
    });
}
QString missionOrigin(const QString &serverId, const QString &name)
{
    return QStringLiteral("mission/%1/%2").arg(serverId, name);
}
bool readMissions(const QByteArray &json, QList<Mission> *missions)
{
    const auto document = QJsonDocument::fromJson(json);
    const auto data = document.object().value(QStringLiteral("data"));
    if (!document.isObject() || !data.isArray() || data.toArray().size() > MaxMissions) {
        return false;
    }
    QList<Mission> result;
    for (const auto &value : data.toArray()) {
        const auto object = value.toObject();
        Mission mission;
        mission.name = object.value(QStringLiteral("name")).toString();
        if (!validMissionName(mission.name)) {
            continue;
        }
        mission.description = plain(object.value(QStringLiteral("description")).toString(), 512);
        mission.creatorUid = plain(object.value(QStringLiteral("creatorUid")).toString());
        mission.tool = plain(object.value(QStringLiteral("tool")).toString(), 64);
        for (const auto &keyword : object.value(QStringLiteral("keywords")).toArray()) {
            if (mission.keywords.size() < 16) {
                mission.keywords.append(plain(keyword.toString(), 64));
            }
        }
        mission.created = readTime(object.value(QStringLiteral("createTime")));
        mission.passwordProtected = object.value(QStringLiteral("passwordProtected")).toBool();
        mission.objects = static_cast<int>(std::min<qsizetype>(
            object.value(QStringLiteral("uids")).toArray().size(), MaxMissionObjects));
        mission.files = static_cast<int>(std::min<qsizetype>(
            object.value(QStringLiteral("contents")).toArray().size(), MaxMissionFiles));
        result.append(mission);
    }
    *missions = result;
    return true;
}
QList<MissionFile> readMissionFiles(const QByteArray &json)
{
    QList<MissionFile> result;
    const auto data = QJsonDocument::fromJson(json).object().value(QStringLiteral("data"));
    const auto mission = data.isArray() ? data.toArray().first().toObject() : data.toObject();
    for (const auto &value : mission.value(QStringLiteral("contents")).toArray()) {
        if (result.size() >= MaxMissionFiles) {
            break;
        }
        const auto item = value.toObject().value(QStringLiteral("data")).toObject();
        MissionFile file;
        file.hash = item.value(QStringLiteral("hash")).toString().toLower();
        if (file.hash.size() != 64 ||
            !std::all_of(file.hash.cbegin(), file.hash.cend(), [](QChar ch) {
                return ch.isDigit() || (ch >= u'a' && ch <= u'f');
            })) {
            continue;
        }
        file.name = plain(item.value(QStringLiteral("name")).toString());
        file.submitter = plain(item.value(QStringLiteral("submitter")).toString());
        const double size = item.value(QStringLiteral("size")).toDouble(-1);
        file.size = size >= 0 && size < 1e15 ? static_cast<qint64>(size) : -1;
        if (file.name.isEmpty()) {
            file.name = file.hash.left(12);
        }
        result.append(file);
    }
    return result;
}
QList<MissionLog> readMissionLogs(const QByteArray &json)
{
    QList<MissionLog> result;
    for (const auto &value :
         QJsonDocument::fromJson(json).object().value(QStringLiteral("data")).toArray()) {
        if (result.size() >= MaxMissionLogs) {
            break;
        }
        const auto object = value.toObject();
        MissionLog entry;
        entry.id = plain(object.value(QStringLiteral("id")).toString());
        entry.creatorUid = plain(object.value(QStringLiteral("creatorUid")).toString());
        entry.content = plain(object.value(QStringLiteral("content")).toString(), 4096);
        entry.time = readTime(object.value(QStringLiteral("dtg")));
        if (!entry.time.isValid()) {
            entry.time = readTime(object.value(QStringLiteral("servertime")));
        }
        if (!entry.content.isEmpty()) {
            result.append(entry);
        }
    }
    std::sort(result.begin(), result.end(), [](const MissionLog &a, const MissionLog &b) {
        return a.time < b.time;
    });
    return result;
}
bool readSubscription(const QByteArray &json, MissionSubscription::Role *role, QString *token)
{
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isObject()) {
        return false;
    }
    const auto data = document.object().value(QStringLiteral("data")).toObject();
    *role = readRole(data.value(QStringLiteral("role")));
    const auto value = data.value(QStringLiteral("token")).toString();
    // A token is a bearer secret for HTTP headers. Refuse unexpected characters.
    if (value.size() > 4096 || std::any_of(value.cbegin(), value.cend(), [](QChar ch) {
            return ch.unicode() < 0x21 || ch.unicode() > 0x7e;
        })) {
        return false;
    }
    *token = value;
    return true;
}
QList<Event> readMissionEvents(const QByteArray &xml)
{
    QList<Event> result;
    qsizetype position = 0;
    while (result.size() < MaxMissionObjects) {
        auto start = xml.indexOf("<event", position);
        while (start >= 0 && start + 6 < xml.size() && xml.at(start + 6) != ' ' &&
               xml.at(start + 6) != '>' && xml.at(start + 6) != '\n' && xml.at(start + 6) != '\t' &&
               xml.at(start + 6) != '\r') {
            start = xml.indexOf("<event", start + 6);
        }
        if (start < 0) {
            break;
        }
        const auto end = xml.indexOf("</event>", start);
        if (end < 0) {
            break;
        }
        position = end + 8;
        const auto decoded = decodeCot(xml.mid(start, position - start));
        if (decoded.event) {
            result.append(*decoded.event);
        }
    }
    return result;
}

Missions::Missions(
    Store &store, Servers *servers, TakServerApi *api, Identity *identity, QObject *parent)
    : QObject(parent), store_(store), servers_(servers), api_(api), identity_(identity)
{
    setObjectName(QStringLiteral("missions"));
    qRegisterMetaType<swak::Mission>();
    connect(&store_, &Store::messageReceived, this, &Missions::receive);
    refresh_.setInterval(RefreshMilliseconds);
    connect(&refresh_, &QTimer::timeout, this, [this] {
        for (const auto &mission : std::as_const(subscriptions_)) {
            reload(mission.serverId, mission.name);
        }
    });
    refresh_.start();
}
QJsonObject Missions::savePreferences() const
{
    QJsonArray saved;
    for (const auto &mission : subscriptions_) {
        saved.append(QJsonObject{{QStringLiteral("server"), mission.serverId},
                                 {QStringLiteral("name"), mission.name},
                                 {QStringLiteral("visible"), mission.visible}});
    }
    return {{QStringLiteral("subscriptions"), saved}};
}
void Missions::restorePreferences(const QJsonObject &values)
{
    const auto entries = values.value(QStringLiteral("subscriptions")).toArray();
    for (const auto &value : entries) {
        if (subscriptions_.size() >= MaxSubscriptions) {
            break;
        }
        const auto object = value.toObject();
        const auto serverId = plain(object.value(QStringLiteral("server")).toString());
        const auto name = object.value(QStringLiteral("name")).toString();
        if (serverId.isEmpty() || !validMissionName(name) || find(serverId, name)) {
            continue;
        }
        MissionSubscription mission;
        mission.serverId = serverId;
        mission.name = name;
        mission.visible = object.value(QStringLiteral("visible")).toBool(true);
        mission.status = tr("Subscribing…");
        subscriptions_.append(mission);
        // Password missions need the password again. The subscribe call reports that.
        QMetaObject::invokeMethod(
            this,
            [this, serverId, name] {
                subscribe(serverId, name);
            },
            Qt::QueuedConnection);
    }
    emit changed();
}
MissionSubscription *Missions::find(const QString &serverId, const QString &name)
{
    const auto found =
        std::find_if(subscriptions_.begin(), subscriptions_.end(), [&](const auto &item) {
            return item.serverId == serverId && item.name == name;
        });
    return found == subscriptions_.end() ? nullptr : &*found;
}
const MissionSubscription *Missions::subscription(const QString &serverId,
                                                  const QString &name) const
{
    const auto found =
        std::find_if(subscriptions_.cbegin(), subscriptions_.cend(), [&](const auto &item) {
            return item.serverId == serverId && item.name == name;
        });
    return found == subscriptions_.cend() ? nullptr : &*found;
}
QString Missions::clientUid() const
{
    Event event;
    if (identity_) {
        identity_->apply(event);
    }
    return event.uid;
}
QList<std::pair<QByteArray, QByteArray>>
Missions::authorization(const MissionSubscription &mission) const
{
    if (mission.token.isEmpty()) {
        return {};
    }
    return {{QByteArrayLiteral("Authorization"), "Bearer " + mission.token.toLatin1()}};
}
void Missions::list(const QString &serverId)
{
    if (!api_) {
        emit listed(serverId, {}, tr("No TAK REST API is available."));
        return;
    }
    api_->martiRequest(
        serverId,
        "GET",
        QStringLiteral("/api/missions?passwordProtected=true"),
        {},
        {},
        this,
        [this, serverId](const NetworkReply &reply) {
            QList<Mission> missions;
            if (!reply.ok()) {
                emit listed(serverId, {}, httpError(reply));
            } else if (!readMissions(reply.body, &missions)) {
                emit listed(serverId, {}, tr("The mission list is invalid."));
            } else {
                emit listed(serverId, missions, {});
            }
        },
        MaxListBytes,
        RequestTimeoutMs);
}
QString Missions::subscribe(const QString &serverId, const QString &name, const QString &password)
{
    if (!api_) {
        return tr("No TAK REST API is available.");
    }
    if (!validMissionName(name)) {
        return tr("The mission name is invalid.");
    }
    const auto uid = clientUid();
    if (uid.isEmpty()) {
        return tr("Set a UID in File > Identity.");
    }
    if (!find(serverId, name)) {
        if (subscriptions_.size() >= MaxSubscriptions) {
            return tr("At most %1 missions can be subscribed.").arg(MaxSubscriptions);
        }
        MissionSubscription mission;
        mission.serverId = serverId;
        mission.name = name;
        mission.status = tr("Subscribing…");
        subscriptions_.append(mission);
        emit changed();
    }
    auto path =
        QStringLiteral("/api/missions/%1/subscription?uid=%2").arg(segment(name), query(uid));
    if (!password.isEmpty()) {
        path += QStringLiteral("&password=") + query(password);
    }
    api_->martiRequest(
        serverId,
        "PUT",
        path,
        {},
        {},
        this,
        [this, serverId, name](const NetworkReply &reply) {
            auto *mission = find(serverId, name);
            if (!mission) {
                return;
            }
            MissionSubscription::Role role = MissionSubscription::Role::Subscriber;
            QString token;
            if (!reply.ok() || !readSubscription(reply.body, &role, &token)) {
                const auto error =
                    reply.ok() ? tr("The subscription reply is invalid.") : httpError(reply);
                mission->status = error;
                emit changed();
                emit subscribed(serverId, name, error);
                return;
            }
            mission->role = role;
            mission->token = token;
            mission->status = tr("Subscribed");
            emit changed();
            emit subscribed(serverId, name, {});
            reload(serverId, name);
        },
        64 * 1024,
        RequestTimeoutMs);
    return {};
}
void Missions::unsubscribe(const QString &serverId, const QString &name)
{
    const auto *mission = find(serverId, name);
    if (!mission) {
        return;
    }
    if (api_) {
        api_->martiRequest(
            serverId,
            "DELETE",
            QStringLiteral("/api/missions/%1/subscription?uid=%2")
                .arg(segment(name), query(clientUid())),
            {},
            {},
            this,
            [](const NetworkReply &) {},
            64 * 1024,
            RequestTimeoutMs);
    }
    hideEvents(*mission);
    events_.remove(missionOrigin(serverId, name));
    delete pendingReloads_.take(missionOrigin(serverId, name));
    subscriptions_.removeIf([&](const auto &item) {
        return item.serverId == serverId && item.name == name;
    });
    emit changed();
}
void Missions::setVisible(const QString &serverId, const QString &name, bool visible)
{
    auto *mission = find(serverId, name);
    if (!mission || mission->visible == visible) {
        return;
    }
    mission->visible = visible;
    const auto copy = *mission;
    if (visible) {
        showEvents(copy);
    } else {
        hideEvents(copy);
    }
    emit changed();
}
void Missions::reload(const QString &serverId, const QString &name)
{
    const auto *mission = find(serverId, name);
    if (!mission || !api_) {
        return;
    }
    const auto headers = authorization(*mission);
    const auto base = QStringLiteral("/api/missions/") + segment(name);
    api_->martiRequest(
        serverId,
        "GET",
        base + QStringLiteral("/cot"),
        {},
        {},
        this,
        [this, serverId, name](const NetworkReply &reply) {
            if (!reply.ok()) {
                if (auto *target = find(serverId, name)) {
                    target->status = httpError(reply);
                    emit changed();
                }
                return;
            }
            loadEvents(serverId, name, reply.body);
        },
        MaxCotBytes,
        RequestTimeoutMs,
        headers);
    api_->martiRequest(
        serverId,
        "GET",
        base,
        {},
        {},
        this,
        [this, serverId, name](const NetworkReply &reply) {
            if (auto *target = find(serverId, name); target && reply.ok()) {
                target->files = readMissionFiles(reply.body);
                emit changed();
            }
        },
        MaxListBytes,
        RequestTimeoutMs,
        headers);
    api_->martiRequest(
        serverId,
        "GET",
        base + QStringLiteral("/log"),
        {},
        {},
        this,
        [this, serverId, name](const NetworkReply &reply) {
            if (auto *target = find(serverId, name); target && reply.ok()) {
                target->logs = readMissionLogs(reply.body);
                emit changed();
            }
        },
        MaxListBytes,
        RequestTimeoutMs,
        headers);
}
void Missions::loadEvents(const QString &serverId, const QString &name, const QByteArray &xml)
{
    auto *mission = find(serverId, name);
    if (!mission) {
        return;
    }
    const auto origin = missionOrigin(serverId, name);
    auto events = readMissionEvents(xml);
    for (auto &event : events) {
        event.origin = origin;
    }
    const auto copy = *mission;
    // Remove objects that left the mission before the new set shows.
    hideEvents(copy);
    events_.insert(origin, events);
    mission = find(serverId, name);
    if (!mission) {
        return;
    }
    mission->objects = static_cast<int>(events.size());
    mission->updated = QDateTime::currentDateTimeUtc();
    mission->status = tr("Updated");
    if (mission->visible) {
        showEvents(*mission);
    }
    emit changed();
}
void Missions::showEvents(const MissionSubscription &mission)
{
    const auto now = QDateTime::currentDateTimeUtc();
    QList<Event> batch;
    for (auto event : events_.value(missionOrigin(mission.serverId, mission.name))) {
        // Mission objects stay until the mission removes them. Keep them current for one day.
        event.time = std::min(event.time, now);
        event.start = std::min(event.start, now);
        event.stale = now.addSecs(MissionLifetimeSeconds);
        batch.append(event);
    }
    store_.acceptBatch(batch);
}
void Missions::hideEvents(const MissionSubscription &mission)
{
    store_.removeOrigin(missionOrigin(mission.serverId, mission.name));
}
QString Missions::addEvent(const QString &serverId, const QString &name, const Event &event)
{
    const auto *mission = find(serverId, name);
    if (!mission) {
        return tr("Subscribe to the mission first.");
    }
    if (!mission->editable()) {
        return tr("Your role in this mission is read-only.");
    }
    if (!servers_) {
        return tr("No output is connected.");
    }
    auto sent = event;
    auto elements = detailElements(sent.detail);
    elements.erase(std::remove_if(elements.begin(),
                                  elements.end(),
                                  [](const CotElement &element) {
                                      return element.name == u"marti";
                                  }),
                   elements.end());
    CotElement marti{QStringLiteral("marti"), {}, {}, {}};
    marti.children.push_back({QStringLiteral("dest"), {{QStringLiteral("mission"), name}}, {}, {}});
    elements.push_back(std::move(marti));
    sent.detail = detailText(elements);
    if (sent.detail.isEmpty()) {
        return tr("The object is too large for a mission.");
    }
    const auto error = servers_->send(sent);
    if (error.isEmpty()) {
        delete pendingReloads_.take(missionOrigin(serverId, name));
        receiveChange(serverId, name);
    }
    return error;
}
QString Missions::removeObject(const QString &serverId, const QString &name, const QString &uid)
{
    const auto *mission = find(serverId, name);
    if (!mission || !api_) {
        return tr("Subscribe to the mission first.");
    }
    if (!mission->editable()) {
        return tr("Your role in this mission is read-only.");
    }
    api_->martiRequest(
        serverId,
        "DELETE",
        QStringLiteral("/api/missions/%1/contents?uid=%2&creatorUid=%3")
            .arg(segment(name), query(uid), query(clientUid())),
        {},
        {},
        this,
        [this, serverId, name](const NetworkReply &reply) {
            if (auto *target = find(serverId, name)) {
                target->status = reply.ok() ? tr("Object removed") : httpError(reply);
                emit changed();
            }
            if (reply.ok()) {
                reload(serverId, name);
            }
        },
        64 * 1024,
        RequestTimeoutMs,
        authorization(*mission));
    return {};
}
QString Missions::addLog(const QString &serverId, const QString &name, const QString &text)
{
    const auto *mission = find(serverId, name);
    const auto content = plain(text, 4096);
    if (!mission || !api_) {
        return tr("Subscribe to the mission first.");
    }
    if (!mission->editable()) {
        return tr("Your role in this mission is read-only.");
    }
    if (content.isEmpty()) {
        return tr("Type a log entry.");
    }
    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QJsonObject entry{
        {QStringLiteral("content"), content},
        {QStringLiteral("creatorUid"), clientUid()},
        {QStringLiteral("entryUid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("missionNames"), QJsonArray{name}},
        {QStringLiteral("dtg"), now},
        {QStringLiteral("servertime"), now}};
    api_->martiRequest(
        serverId,
        "POST",
        QStringLiteral("/api/missions/logs/entries"),
        QJsonDocument(entry).toJson(QJsonDocument::Compact),
        "application/json",
        this,
        [this, serverId, name](const NetworkReply &reply) {
            if (auto *target = find(serverId, name)) {
                target->status = reply.ok() ? tr("Log entry added") : httpError(reply);
                emit changed();
            }
            if (reply.ok()) {
                reload(serverId, name);
            }
        },
        64 * 1024,
        RequestTimeoutMs,
        authorization(*mission));
    return {};
}
QString Missions::exportArchive(const QString &serverId, const QString &name)
{
    const auto *mission = find(serverId, name);
    if (!mission || !api_) {
        return tr("Subscribe to the mission first.");
    }
    api_->martiRequest(
        serverId,
        "GET",
        QStringLiteral("/api/missions/%1/archive").arg(segment(name)),
        {},
        {},
        this,
        [this, serverId, name](const NetworkReply &reply) {
            emit exported(serverId,
                          name,
                          reply.ok() ? reply.body : QByteArray{},
                          reply.ok() ? QString{} : httpError(reply));
        },
        MaxArchiveBytes,
        600000,
        authorization(*mission));
    return {};
}
void Missions::receiveChange(const QString &serverId, const QString &name)
{
    // Changes often come in groups. One reload follows the last change of a group.
    const auto key = missionOrigin(serverId, name);
    auto *timer = pendingReloads_.value(key);
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(ReloadDelayMilliseconds);
        connect(timer, &QTimer::timeout, this, [this, serverId, name, key] {
            delete pendingReloads_.take(key);
            reload(serverId, name);
        });
        pendingReloads_.insert(key, timer);
    }
    timer->start();
}
void Missions::receive(const Event &event)
{
    if (!event.type.startsWith(u"t-x-m-") || event.origin.isEmpty()) {
        return;
    }
    const auto element = detailElement(event, u"mission");
    const auto name = element ? element->attribute(u"name") : QString{};
    if (!validMissionName(name)) {
        return;
    }
    const auto serverId = event.origin;
    if (event.type == u"t-x-m-c") {
        if (find(serverId, name)) {
            receiveChange(serverId, name);
        }
    } else if (event.type == u"t-x-m-d") {
        if (const auto *mission = find(serverId, name)) {
            hideEvents(*mission);
            events_.remove(missionOrigin(serverId, name));
            delete pendingReloads_.take(missionOrigin(serverId, name));
            subscriptions_.removeIf([&](const auto &item) {
                return item.serverId == serverId && item.name == name;
            });
            emit changed();
        }
    } else if (event.type == u"t-x-m-i") {
        const std::pair entry{serverId, name};
        if (!invitations_.contains(entry)) {
            if (invitations_.size() >= MaxSubscriptions) {
                invitations_.removeFirst();
            }
            invitations_.append(entry);
            emit invited(serverId, name);
            emit changed();
        }
    }
}
} // namespace swak

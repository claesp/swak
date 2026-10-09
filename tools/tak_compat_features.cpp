#include "tak_compat_features.h"

#include "core/chat.h"
#include "core/citrap.h"
#include "core/contacts.h"
#include "core/cot.h"
#include "core/cot_detail.h"
#include "core/data_feeds.h"
#include "core/data_package.h"
#include "core/emergency.h"
#include "core/excheck.h"
#include "core/file_shares.h"
#include "core/icon_cache.h"
#include "core/identity.h"
#include "core/missions.h"
#include "core/network_client.h"
#include "core/object_sharing.h"
#include "core/projection.h"
#include "core/server_layers.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/tak_channels.h"
#include "core/tak_drawings.h"
#include "core/tak_enrollment.h"
#include "core/tak_server_api.h"
#include "core/tak_shapes.h"
#include "core/track_history.h"
#include "core/user_objects.h"
#include "core/video_sources.h"

#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <memory>

using namespace swak;
namespace {
constexpr int Wait = 20000;

// One client with the production services of a SWAK window.
struct Client
{
    Store store;
    Servers *servers;
    TakServerApi *api;
    Identity identity;
    UserObjects objects;
    Client(Servers &stream, NetworkClient &network, const Event &self)
        : servers(&stream), api(stream.findChild<TakServerApi *>())
    {
        if (!api) {
            api = new TakServerApi(stream, network, &stream);
            ownsApi = true;
        }
        auto values = identity.savePreferences();
        values.insert(QStringLiteral("uid"), self.uid);
        values.insert(QStringLiteral("callsign"), self.callsign);
        values.insert(QStringLiteral("type"), self.type);
        values.insert(QStringLiteral("team"), self.team);
        values.insert(QStringLiteral("role"), self.role);
        identity.restorePreferences(values);
        stream.setSenderIdentity(self.uid, self.callsign);
    }
    // The API uses the network client of the checks, so it must end with them.
    ~Client()
    {
        if (ownsApi) {
            delete api;
        }
    }
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    bool ownsApi = false;
};
// Waits for one reply of a raw REST request.
NetworkReply call(TakServerApi &api,
                  const QByteArray &method,
                  const QString &path,
                  const QByteArray &body = {},
                  const QByteArray &type = {})
{
    NetworkReply result;
    bool done = false;
    QObject context;
    api.martiRequest(
        QStringLiteral("test"),
        method,
        path,
        body,
        type,
        &context,
        [&](const NetworkReply &reply) {
            result = reply;
            done = true;
        },
        8 * 1024 * 1024,
        60000);
    static_cast<void>(QTest::qWaitFor(
        [&] {
            return done;
        },
        70000));
    return result;
}
QString replyText(const NetworkReply &reply)
{
    return reply.error.isEmpty() ? QStringLiteral("HTTP %1").arg(reply.status) : reply.error;
}
bool has(const Store &store, const QString &uid)
{
    return std::any_of(store.events().cbegin(), store.events().cend(), [&uid](const Event &event) {
        return event.uid == uid;
    });
}
const Event *find(const Store &store, const QString &uid)
{
    const auto found =
        std::find_if(store.events().cbegin(), store.events().cend(), [&uid](const Event &event) {
            return event.uid == uid;
        });
    return found == store.events().cend() ? nullptr : &*found;
}
UserObject area(const QString &id)
{
    UserObject item;
    item.id = id;
    item.type = QStringLiteral("polygon");
    item.title = QStringLiteral("SWAK test area");
    QJsonArray vertices;
    for (const auto &[latitude, longitude] :
         {std::pair{0.10, 0.20}, std::pair{0.10, 0.30}, std::pair{0.20, 0.30}}) {
        const auto point = project(latitude, longitude);
        vertices.append(QJsonArray{point.x(), point.y()});
    }
    item.geometry = {{QStringLiteral("vertices"), vertices}};
    item.color = QStringLiteral("#00a0ff");
    return item;
}
} // namespace

QJsonObject runFeatureChecks(Servers &left,
                             Servers &right,
                             Store &leftStore,
                             Store &rightStore,
                             const Event &a,
                             const Event &b,
                             const QJsonObject &profile)
{
    QJsonObject checks;
    const auto record = [&checks](const QString &name, bool passed, const QString &detail) {
        checks.insert(
            name,
            QJsonObject{{QStringLiteral("passed"), passed}, {QStringLiteral("detail"), detail}});
    };
    NetworkClient network;
    Client one(left, network, a);
    Client two(right, network, b);
    const auto id = [](const QString &prefix) {
        return prefix + QUuid::createUuid().toString(QUuid::Id128).left(12);
    };

    // REST API and version check after the TLS connection.
    {
        const bool ok = QTest::qWaitFor(
            [&] {
                return one.api->info(QStringLiteral("test")).available();
            },
            Wait);
        const auto info = one.api->info(QStringLiteral("test"));
        record(QStringLiteral("version"), ok, info.version + QLatin1Char(' ') + info.error);
    }
    // Channels: read the groups and apply the same active set.
    {
        TakChannels channels(*one.api);
        QSignalSpy loaded(&channels, &TakChannels::loaded);
        QSignalSpy applied(&channels, &TakChannels::applied);
        channels.load(QStringLiteral("test"));
        bool ok = loaded.wait(Wait) && loaded.last().at(2).toString().isEmpty();
        QString detail = ok                 ? QString{}
                         : loaded.isEmpty() ? QStringLiteral("no reply")
                                            : loaded.last().at(2).toString();
        if (ok) {
            const auto groups = loaded.last().at(1).value<QList<TakGroup>>();
            const auto names = channelNames(groups);
            detail = names.join(QStringLiteral(", "));
            channels.apply(QStringLiteral("test"), groups, activeChannels(groups), a.uid);
            ok = names.contains(QStringLiteral("SwakTest")) && applied.wait(Wait) &&
                 applied.last().at(1).toString().isEmpty();
            if (!applied.isEmpty() && !applied.last().at(1).toString().isEmpty()) {
                detail += QStringLiteral("; apply: ") + applied.last().at(1).toString();
            }
        }
        record(QStringLiteral("channels"), ok, detail);
    }
    // Contacts: the server lists the other connected user.
    {
        Contacts contacts(leftStore, &left, one.api);
        bool found = false;
        QString error;
        static_cast<void>(QTest::qWaitFor(
            [&] {
                QSignalSpy refreshed(&contacts, &Contacts::refreshed);
                contacts.refreshServers();
                if (!refreshed.wait(Wait)) {
                    return true;
                }
                error = refreshed.last().at(0).toString();
                found = std::any_of(contacts.contacts().cbegin(),
                                    contacts.contacts().cend(),
                                    [&b](const Contact &contact) {
                                        return contact.uid == b.uid;
                                    });
                if (!found) {
                    QTest::qWait(1000);
                }
                return found || !error.isEmpty();
            },
            Wait));
        record(QStringLiteral("contacts"),
               found,
               error.isEmpty() ? QStringLiteral("%1 contacts").arg(contacts.contacts().size())
                               : error);
    }
    // GeoChat: a direct message and its delivered receipt.
    {
        Chat leftChat(leftStore, &left, &one.identity);
        Chat rightChat(rightStore, &right, &two.identity);
        const auto room = leftChat.openDirect(b.uid, b.callsign);
        const auto error = leftChat.send(room, QStringLiteral("SWAK compatibility message"));
        const bool received =
            error.isEmpty() && QTest::qWaitFor(
                                   [&] {
                                       const auto *direct = rightChat.room(a.uid);
                                       return direct && !direct->messages.isEmpty();
                                   },
                                   Wait);
        const bool delivered =
            received && QTest::qWaitFor(
                            [&] {
                                const auto *own = leftChat.room(b.uid);
                                return own && !own->messages.isEmpty() &&
                                       own->messages.last().state >= ChatMessage::State::Delivered;
                            },
                            Wait);
        record(QStringLiteral("chatDirect"), received, error.isEmpty() ? QString{} : error);
        record(QStringLiteral("chatReceipt"), delivered, {});
        const auto group = leftChat.send(AllChatRooms, QStringLiteral("SWAK all rooms"));
        const bool all =
            group.isEmpty() && QTest::qWaitFor(
                                   [&] {
                                       const auto *everyone = rightChat.room(AllChatRooms);
                                       return everyone && !everyone->messages.isEmpty();
                                   },
                                   Wait);
        record(QStringLiteral("chatAllRooms"), all, group);
    }
    // Shared drawing and its delete.
    {
        ObjectSharing sharing(one.objects, &left, &one.identity);
        const auto item = area(id(QStringLiteral("swak-area-")));
        auto error = one.objects.put(item);
        if (error.isEmpty()) {
            error = sharing.share(item.id);
        }
        const bool shown =
            error.isEmpty() && QTest::qWaitFor(
                                   [&] {
                                       const auto *event = find(rightStore, item.id);
                                       return event && readTakDrawing(*event).has_value();
                                   },
                                   Wait);
        record(
            QStringLiteral("shareDrawing"), shown, error.isEmpty() ? sharing.lastError() : error);
        sharing.stopSharing(item.id);
        const bool removed = shown && QTest::qWaitFor(
                                          [&] {
                                              return !has(rightStore, item.id);
                                          },
                                          Wait);
        record(QStringLiteral("deleteDrawing"), removed, {});
    }
    // Emergency alert and cancel.
    {
        auto sender = a;
        sender.time = sender.start = QDateTime::currentDateTimeUtc();
        sender.stale = sender.time.addSecs(180);
        const auto alert = emergencyEvent(sender, QStringLiteral("911 Alert"));
        auto error = left.send(alert);
        const bool raised = error.isEmpty() && QTest::qWaitFor(
                                                   [&] {
                                                       const auto *event =
                                                           find(rightStore, alert.uid);
                                                       return event && activeEmergency(*event);
                                                   },
                                                   Wait);
        error = left.send(emergencyCancelEvent(sender));
        const bool cancelled = raised && error.isEmpty() &&
                               QTest::qWaitFor(
                                   [&] {
                                       return !has(rightStore, alert.uid);
                                   },
                                   Wait);
        record(QStringLiteral("emergency"), raised, {});
        record(QStringLiteral("emergencyCancel"), cancelled, error);
    }
    // Data package upload, offer, download, and import.
    {
        QTemporaryDir folder;
        FileShares sender(leftStore, &left, one.api, &one.identity, &one.objects, nullptr, nullptr);
        FileShares receiver(
            rightStore, &right, two.api, &two.identity, &two.objects, nullptr, nullptr);
        receiver.setDirectory(folder.path());
        const auto item = area(id(QStringLiteral("swak-package-")));
        QSignalSpy sent(&sender, &FileShares::sent);
        auto error = one.objects.put(item);
        if (error.isEmpty()) {
            error = sender.send(QStringLiteral("test"),
                                QStringLiteral("SWAK package"),
                                {item.id},
                                {{b.uid, b.callsign}});
        }
        const bool uploaded =
            error.isEmpty() && sent.wait(Wait) && sent.last().at(0).toString().isEmpty();
        if (error.isEmpty() && !sent.isEmpty()) {
            error = sent.last().at(0).toString();
        }
        record(QStringLiteral("packageUpload"), uploaded, error);
        const bool offered = uploaded && QTest::qWaitFor(
                                             [&] {
                                                 return !receiver.offers().isEmpty();
                                             },
                                             Wait);
        record(QStringLiteral("packageOffer"), offered, {});
        QString imported;
        if (offered) {
            const auto offer = receiver.offers().first();
            receiver.download(offer.serverId, offer.hash);
            static_cast<void>(QTest::qWaitFor(
                [&] {
                    const auto state = receiver.offers().first().state;
                    return state == FileOffer::State::Imported || state == FileOffer::State::Failed;
                },
                Wait));
            imported = receiver.offers().first().status;
        }
        record(QStringLiteral("packageImport"),
               offered && receiver.offers().first().state == FileOffer::State::Imported &&
                   two.objects.find(item.id).has_value(),
               imported);
        QSignalSpy searched(&receiver, &FileShares::searched);
        receiver.search(QStringLiteral("test"));
        const bool listed = searched.wait(Wait) && searched.last().at(2).toString().isEmpty();
        record(QStringLiteral("serverFiles"),
               listed,
               searched.isEmpty() ? QString{} : searched.last().at(2).toString());
    }
    // Missions: create, subscribe, add an object, write the log, export, and delete.
    {
        const auto name = id(QStringLiteral("swak-mission-"));
        const auto created = call(
            *one.api, "PUT", QStringLiteral("/api/missions/%1?creatorUid=%2").arg(name, a.uid));
        record(QStringLiteral("missionCreate"), created.ok(), replyText(created));
        Missions missions(leftStore, &left, one.api, &one.identity);
        QSignalSpy subscribed(&missions, &Missions::subscribed);
        missions.subscribe(QStringLiteral("test"), name);
        const bool joined = subscribed.wait(Wait) && subscribed.last().at(2).toString().isEmpty();
        record(QStringLiteral("missionSubscribe"),
               joined,
               subscribed.isEmpty() ? QString{} : subscribed.last().at(2).toString());
        auto sender = a;
        const auto item = area(id(QStringLiteral("swak-mission-area-")));
        const auto event = sharedObjectEvent(item, sender, {}, QDateTime::currentDateTimeUtc());
        const auto added = joined && event ? missions.addEvent(QStringLiteral("test"), name, *event)
                                           : QStringLiteral("not subscribed");
        const bool hasObject =
            added.isEmpty() && QTest::qWaitFor(
                                   [&] {
                                       missions.reload(QStringLiteral("test"), name);
                                       QTest::qWait(1500);
                                       const auto *mission =
                                           missions.subscription(QStringLiteral("test"), name);
                                       return mission && mission->objects > 0;
                                   },
                                   Wait);
        record(QStringLiteral("missionAddObject"), hasObject, added);
        const auto logged =
            joined ? missions.addLog(QStringLiteral("test"), name, QStringLiteral("SWAK log entry"))
                   : QStringLiteral("not subscribed");
        const bool hasLog = logged.isEmpty() && QTest::qWaitFor(
                                                    [&] {
                                                        const auto *mission = missions.subscription(
                                                            QStringLiteral("test"), name);
                                                        return mission && !mission->logs.isEmpty();
                                                    },
                                                    Wait);
        const auto *mission = missions.subscription(QStringLiteral("test"), name);
        record(QStringLiteral("missionLog"), hasLog, mission ? mission->status : logged);
        QSignalSpy exported(&missions, &Missions::exported);
        missions.exportArchive(QStringLiteral("test"), name);
        const bool archive =
            exported.wait(Wait) && exported.last().at(2).toByteArray().startsWith("PK");
        record(QStringLiteral("missionExport"),
               archive,
               exported.isEmpty() ? QString{} : exported.last().at(3).toString());
        missions.unsubscribe(QStringLiteral("test"), name);
        const auto removed = call(
            *one.api, "DELETE", QStringLiteral("/api/missions/%1?creatorUid=%2").arg(name, a.uid));
        record(QStringLiteral("missionDelete"), removed.ok(), replyText(removed));
    }
    // ExCheck: the active list and the templates answer.
    {
        ExCheck excheck(one.api, &one.identity);
        QSignalSpy listed(&excheck, &ExCheck::listed);
        excheck.list(QStringLiteral("test"));
        const bool ok = listed.wait(Wait) && listed.last().at(3).toString().isEmpty();
        record(QStringLiteral("excheckList"),
               ok,
               listed.isEmpty() ? QString{} : listed.last().at(3).toString());
    }
    // CI-TRAP: create with an attachment, list, and update.
    {
        Citrap citrap(one.api, &one.identity);
        CitrapReport report;
        report.title = QStringLiteral("SWAK compatibility report");
        report.type = QStringLiteral("Observation");
        report.importance = QStringLiteral("Low");
        report.description = QStringLiteral("Synthetic test report");
        report.latitude = 0.1;
        report.longitude = 0.2;
        QSignalSpy created(&citrap, &Citrap::created);
        auto error =
            citrap.create(QStringLiteral("test"), report, {{QStringLiteral("note.txt"), "test"}});
        const bool made =
            error.isEmpty() && created.wait(Wait) && created.last().at(2).toString().isEmpty();
        const auto reportId = created.isEmpty() ? QString{} : created.last().at(1).toString();
        record(QStringLiteral("citrapCreate"),
               made,
               error.isEmpty() && !created.isEmpty() ? created.last().at(2).toString() : error);
        QSignalSpy listed(&citrap, &Citrap::listed);
        citrap.list(QStringLiteral("test"));
        QList<CitrapReport> reports;
        const bool listedOk = listed.wait(Wait) && listed.last().at(2).toString().isEmpty();
        if (listedOk) {
            reports = listed.last().at(1).value<QList<CitrapReport>>();
        }
        const auto found =
            std::find_if(reports.cbegin(), reports.cend(), [&reportId](const auto &item) {
                return item.id == reportId;
            });
        record(QStringLiteral("citrapList"),
               listedOk && found != reports.cend(),
               listed.isEmpty() ? QString{} : listed.last().at(2).toString());
        if (found != reports.cend()) {
            auto changed = *found;
            changed.status = QStringLiteral("Closed");
            QSignalSpy updated(&citrap, &Citrap::updated);
            error = citrap.update(QStringLiteral("test"), changed);
            const bool ok =
                error.isEmpty() && updated.wait(Wait) && updated.last().at(2).toString().isEmpty();
            record(QStringLiteral("citrapUpdate"),
                   ok,
                   error.isEmpty() && !updated.isEmpty() ? updated.last().at(2).toString() : error);
        }
    }
    // Video: create a connection through REST and read it with the video list.
    {
        const auto uid = id(QStringLiteral("swak-video-"));
        const auto body =
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("videoConnections"),
                     QJsonArray{QJsonObject{
                         {QStringLiteral("uid"), uid},
                         {QStringLiteral("alias"), QStringLiteral("SWAK gate")},
                         {QStringLiteral("active"), true},
                         {QStringLiteral("feeds"),
                          QJsonArray{QJsonObject{
                              {QStringLiteral("uuid"), uid + QStringLiteral("-feed")},
                              {QStringLiteral("alias"), QStringLiteral("SWAK gate")},
                              {QStringLiteral("url"), QStringLiteral("rtsp://10.0.0.9:554/live")},
                              {QStringLiteral("active"), true}}}}}}}})
                .toJson(QJsonDocument::Compact);
        const auto posted =
            call(*one.api, "POST", QStringLiteral("/api/video"), body, "application/json");
        record(QStringLiteral("videoCreate"), posted.ok(), replyText(posted));
        VideoSources sources(leftStore, &left, one.api);
        QSignalSpy refreshed(&sources, &VideoSources::refreshed);
        sources.refresh();
        const bool ok = refreshed.wait(Wait) && refreshed.last().at(0).toString().isEmpty() &&
                        std::any_of(sources.sources().cbegin(),
                                    sources.sources().cend(),
                                    [](const auto &source) {
                                        return source.url.host() == QStringLiteral("10.0.0.9");
                                    });
        record(QStringLiteral("videoList"),
               ok,
               refreshed.isEmpty() ? QString{} : refreshed.last().at(0).toString());
        call(*one.api, "DELETE", QStringLiteral("/api/video/") + uid);
    }
    // Data feeds and map layers parse.
    {
        const auto feeds = call(*one.api, "GET", QStringLiteral("/api/datafeeds"));
        QList<ServerFeed> list;
        record(QStringLiteral("dataFeeds"),
               feeds.ok() && readServerFeeds(feeds.body, &list),
               replyText(feeds));
        const auto layers = call(*one.api, "GET", QStringLiteral("/api/maplayers/all"));
        QList<ServerMapLayer> parsed;
        QStringList skipped;
        record(QStringLiteral("mapLayers"),
               layers.ok() &&
                   readServerMapLayers(layers.body, QStringLiteral("test"), {}, &parsed, &skipped),
               replyText(layers));
    }
    // History: the positions of the left client from the last hour.
    {
        TrackHistory history(&left, one.api);
        QSignalSpy loaded(&history, &TrackHistory::loaded);
        const auto end = QDateTime::currentDateTimeUtc().addSecs(60);
        history.load(QStringLiteral("test"), a.uid, a.callsign, end.addSecs(-3600), end);
        const bool ok = loaded.wait(Wait) && loaded.last().at(1).toString().isEmpty();
        record(QStringLiteral("history"),
               ok,
               ok ? QStringLiteral("%1 positions").arg(history.track(a.uid)->points.size())
               : loaded.isEmpty() ? QStringLiteral("no reply")
                                  : loaded.last().at(1).toString());
    }
    // ExCheck: create a template, start a checklist, and complete one task.
    {
        const auto templateUid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto taskUid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto templateXml =
            QStringLiteral(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><checklist><checklistDetails>"
                "<name>SWAK template</name><uid>%1</uid><description>Probe template</description>"
                "<creatorCallsign>%2</creatorCallsign><creatorUid>%3</creatorUid></"
                "checklistDetails>"
                "<checklistColumns><checklistColumn><columnName>Step</columnName>"
                "<columnType>ShortString</columnType><columnEditable>false</columnEditable>"
                "<columnWidth>200</columnWidth></checklistColumn></checklistColumns>"
                "<checklistTasks><checklistTask><uid>%4</uid><value>Radio check</value>"
                "<number>1</number></checklistTask></checklistTasks></checklist>")
                .arg(templateUid, a.callsign, a.uid, taskUid)
                .toUtf8();
        const auto posted = call(*one.api,
                                 "POST",
                                 QStringLiteral("/api/excheck/template?clientUid=%1").arg(a.uid),
                                 templateXml,
                                 "application/xml");
        record(QStringLiteral("excheckTemplate"), posted.ok(), replyText(posted));
        ExCheck excheck(one.api, &one.identity);
        QSignalSpy listed(&excheck, &ExCheck::listed);
        excheck.list(QStringLiteral("test"));
        const bool hasTemplate =
            listed.wait(Wait) &&
            std::any_of(listed.last().at(2).value<QList<ChecklistTemplate>>().cbegin(),
                        listed.last().at(2).value<QList<ChecklistTemplate>>().cend(),
                        [&templateUid](const ChecklistTemplate &item) {
                            return item.uid == templateUid;
                        });
        record(QStringLiteral("excheckTemplateList"), hasTemplate, {});
        QSignalSpy started(&excheck, &ExCheck::started);
        const auto title = id(QStringLiteral("SWAK checklist "));
        auto error =
            excheck.start(QStringLiteral("test"), templateUid, title, QStringLiteral("Probe run"));
        const bool running =
            error.isEmpty() && started.wait(Wait) && started.last().at(1).toString().isEmpty();
        record(QStringLiteral("excheckStart"),
               running,
               error.isEmpty() && !started.isEmpty() ? started.last().at(1).toString() : error);
        Checklist checklist;
        if (running) {
            excheck.list(QStringLiteral("test"));
            if (listed.wait(Wait)) {
                for (const auto &item : listed.last().at(1).value<QList<Checklist>>()) {
                    if (item.name == title) {
                        checklist = item;
                    }
                }
            }
        }
        QSignalSpy loaded(&excheck, &ExCheck::loaded);
        if (!checklist.uid.isEmpty()) {
            excheck.load(QStringLiteral("test"), checklist.uid);
            if (loaded.wait(Wait) && loaded.last().at(2).toString().isEmpty()) {
                checklist = loaded.last().at(1).value<Checklist>();
            }
        }
        record(QStringLiteral("excheckLoad"),
               !checklist.tasks.isEmpty(),
               QStringLiteral("%1 tasks").arg(checklist.tasks.size()));
        bool completed = false;
        if (!checklist.tasks.isEmpty()) {
            QSignalSpy updated(&excheck, &ExCheck::updated);
            error = excheck.setStatus(QStringLiteral("test"),
                                      checklist.uid,
                                      checklist.tasks.first(),
                                      QStringLiteral("Complete"));
            if (error.isEmpty() && updated.wait(Wait) &&
                updated.last().at(2).toString().isEmpty()) {
                excheck.load(QStringLiteral("test"), checklist.uid);
                completed = loaded.wait(Wait) &&
                            !loaded.last().at(1).value<Checklist>().tasks.isEmpty() &&
                            loaded.last().at(1).value<Checklist>().tasks.first().complete();
                if (!completed && !loaded.isEmpty() &&
                    !loaded.last().at(1).value<Checklist>().tasks.isEmpty()) {
                    error = QStringLiteral("read back: ") +
                            loaded.last().at(1).value<Checklist>().tasks.first().status;
                }
            } else if (error.isEmpty() && !updated.isEmpty()) {
                error = updated.last().at(2).toString();
            }
        }
        record(QStringLiteral("excheckTaskUpdate"), completed, error);
        if (!checklist.uid.isEmpty()) {
            call(
                *one.api,
                "DELETE",
                QStringLiteral("/api/excheck/checklist/%1?clientUid=%2").arg(checklist.uid, a.uid));
        }
        call(*one.api,
             "DELETE",
             QStringLiteral("/api/excheck/template/%1?clientUid=%2").arg(templateUid, a.uid));
    }
    // Iconsets: upload one, then show its icon through the icon cache.
    {
        const auto uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        const auto iconsetXml =
            QStringLiteral(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><iconset version=\"1\" name=\"SWAK "
                "test\" uid=\"%1\" skipResize=\"false\" defaultFriendly=\"\" defaultHostile=\"\" "
                "defaultUnknown=\"\"/>")
                .arg(uid)
                .toUtf8();
        QString error;
        const auto zip = writeDataPackage(uid,
                                          QStringLiteral("SWAK test"),
                                          {{QStringLiteral("iconset.xml"), iconsetXml},
                                           {QStringLiteral("Probe/marker.png"), png}},
                                          &error);
        const auto boundary = QUuid::createUuid().toString(QUuid::Id128).toLatin1();
        const QByteArray body =
            "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"iconset.zip\"\r\n"
            "Content-Type: application/zip\r\n\r\n" +
            zip + "\r\n--" + boundary + "--\r\n";
        // TAK Server lets only administrators upload iconsets.
        Store adminStore;
        Servers admin(adminStore);
        auto adminEntry = serverDefaults();
        const auto adminProfile = profile.value(QStringLiteral("admin")).toObject();
        for (auto it = adminProfile.begin(); it != adminProfile.end(); ++it) {
            adminEntry.insert(it.key(), it.value());
        }
        adminEntry.insert(QStringLiteral("name"), QStringLiteral("Admin"));
        adminEntry.insert(QStringLiteral("enabled"), false);
        error += admin.put(QStringLiteral("test"), adminEntry);
        TakServerApi adminApi(admin, network);
        const auto posted = call(adminApi,
                                 "POST",
                                 QStringLiteral("/api/iconset"),
                                 body,
                                 "multipart/form-data; boundary=" + boundary);
        record(QStringLiteral("iconsetUpload"),
               posted.ok() && error.isEmpty(),
               replyText(posted) + error);
        const auto path = uid + QStringLiteral("/Probe/marker.png");
        // The server processes uploads in the background and caches a missing icon. Wait until
        // the iconset lists the icon, then ask once.
        static_cast<void>(QTest::qWaitFor(
            [&] {
                return replyText(call(adminApi, "GET", QStringLiteral("/api/iconseturl/") + uid))
                    .contains(QStringLiteral("marker.png"));
            },
            Wait));
        QImage icon;
        IconCache cache(&left, one.api);
        QSignalSpy arrived(&cache, &IconCache::iconLoaded);
        if (cache.icon(QStringLiteral("test"), path).isNull() && arrived.wait(Wait)) {
            icon = cache.icon(QStringLiteral("test"), path);
        }
        record(QStringLiteral("iconsetIcon"),
               !icon.isNull() && icon.pixelColor(8, 8).blue() > 200,
               icon.isNull() ? QStringLiteral("no icon")
                             : QStringLiteral("%1x%2").arg(icon.width()).arg(icon.height()));
    }
    // QUIC: a third client exchanges CoT with the TLS client on the right.
    const auto quicProfile = profile.value(QStringLiteral("quic")).toObject();
    if (!quicProfile.isEmpty()) {
        Store store;
        Servers quic(store);
        auto entry = serverDefaults();
        for (auto it = quicProfile.begin(); it != quicProfile.end(); ++it) {
            entry.insert(it.key(), it.value());
        }
        entry.insert(QStringLiteral("name"), QStringLiteral("QUIC"));
        entry.insert(QStringLiteral("enabled"), true);
        auto error = quic.put(QStringLiteral("test"), entry);
        const bool connected = error.isEmpty() && QTest::qWaitFor(
                                                      [&] {
                                                          return quic.statistics()
                                                              .value(QStringLiteral("test"))
                                                              .toObject()
                                                              .value(QStringLiteral("connected"))
                                                              .toBool();
                                                      },
                                                      Wait);
        record(QStringLiteral("quicConnect"),
               connected,
               error.isEmpty() ? quic.status(QStringLiteral("test")) : error);
        auto sent = a;
        sent.uid += QStringLiteral("-QUIC");
        sent.time = sent.start = QDateTime::currentDateTimeUtc();
        sent.stale = sent.time.addSecs(180);
        auto back = b;
        back.uid += QStringLiteral("-TO-QUIC");
        back.time = back.start = sent.time;
        back.stale = sent.stale;
        const bool exchanged = connected && quic.send(sent).isEmpty() &&
                               right.send(back).isEmpty() &&
                               QTest::qWaitFor(
                                   [&] {
                                       return has(rightStore, sent.uid) && has(store, back.uid);
                                   },
                                   Wait);
        record(QStringLiteral("quicExchange"), exchanged, quic.status(QStringLiteral("test")));
    }
    // Enrollment: request a certificate with a user name and password, then connect with it.
    const auto enrollment = profile.value(QStringLiteral("enrollment")).toObject();
    if (!enrollment.isEmpty()) {
        QTemporaryDir folder;
        Store store;
        Servers servers(store);
        const auto enrolled = enrollClient(servers,
                                           network,
                                           profile.value(QStringLiteral("left")).toObject(),
                                           enrollment,
                                           a.uid,
                                           folder.path());
        record(QStringLiteral("enrollment"), enrolled.signedIn, enrolled.error);
        if (enrolled.signedIn) {
            record(QStringLiteral("enrolledConnection"), enrolled.connected, enrolled.status);
        }
    }
    return checks;
}

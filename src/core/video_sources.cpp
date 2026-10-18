#include "core/video_sources.h"

#include "core/cot_detail.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/tak_server_api.h"
#include "core/text.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QXmlStreamReader>

#include <algorithm>
#include <memory>

namespace swak {
namespace {
constexpr qsizetype MaxListBytes = 1024 * 1024;
constexpr int MaxFeeds = 256;

QString plain(const QString &text)
{
    return plainText(text, 128);
}
} // namespace

VideoProtocol videoProtocol(const QUrl &url, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return VideoProtocol::Unsupported;
    };
    if (!url.isValid() || url.toString().size() > 2048 || url.hasFragment()) {
        return fail(VideoSources::tr("The video URL is invalid."));
    }
    if (url.port() == 0) {
        return fail(VideoSources::tr("The video URL has an invalid port."));
    }
    const auto scheme = url.scheme().toLower();
    VideoProtocol protocol = VideoProtocol::Unsupported;
    if (scheme == u"rtsp" || scheme == u"rtsps") {
        protocol = VideoProtocol::Rtsp;
    } else if (scheme == u"rtmp" || scheme == u"rtmps") {
        protocol = VideoProtocol::Rtmp;
    } else if (scheme == u"srt") {
        protocol = VideoProtocol::Srt;
    } else if (scheme == u"http" || scheme == u"https") {
        protocol = VideoProtocol::Hls;
    } else if (scheme == u"udp") {
        // MPEG-TS over UDP can listen on any address, such as udp://@:1234.
        if (url.port() < 0) {
            return fail(VideoSources::tr("A UDP video URL needs a port."));
        }
        return VideoProtocol::Udp;
    } else {
        return fail(VideoSources::tr("SWAK plays RTSP, RTMP, SRT, HLS over HTTP or HTTPS, and "
                                     "MPEG-TS over UDP."));
    }
    if (url.host().isEmpty()) {
        return fail(VideoSources::tr("The video URL needs a host."));
    }
    return protocol;
}
QString videoProtocolName(VideoProtocol protocol)
{
    switch (protocol) {
    case VideoProtocol::Rtsp:
        return QStringLiteral("RTSP");
    case VideoProtocol::Rtmp:
        return QStringLiteral("RTMP");
    case VideoProtocol::Srt:
        return QStringLiteral("SRT");
    case VideoProtocol::Hls:
        return QStringLiteral("HLS");
    case VideoProtocol::Udp:
        return QStringLiteral("UDP");
    case VideoProtocol::Unsupported:
        return VideoSources::tr("Unsupported");
    }
    return {};
}
namespace {
// Reads the JSON form: {"videoConnections":[{"alias":..., "feeds":[{"alias":..., "url":...}]}]}.
bool readVideoJson(const QJsonObject &root, const QString &origin, QList<VideoSource> *sources)
{
    const auto connections = root.value(QStringLiteral("videoConnections"));
    if (!connections.isArray() || connections.toArray().size() > MaxFeeds) {
        return false;
    }
    QList<VideoSource> result;
    int feeds = 0;
    for (const auto &value : connections.toArray()) {
        const auto connection = value.toObject();
        for (const auto &entry : connection.value(QStringLiteral("feeds")).toArray()) {
            if (++feeds > MaxFeeds) {
                return false;
            }
            const auto feed = entry.toObject();
            const QUrl url(feed.value(QStringLiteral("url")).toString().trimmed(),
                           QUrl::StrictMode);
            const auto protocol = videoProtocol(url);
            if (protocol == VideoProtocol::Unsupported || result.size() >= MaxVideoSources ||
                !feed.value(QStringLiteral("active")).toBool(true)) {
                continue;
            }
            auto name = plain(feed.value(QStringLiteral("alias")).toString());
            if (name.isEmpty()) {
                name = plain(connection.value(QStringLiteral("alias")).toString());
            }
            if (name.isEmpty()) {
                name = url.host();
            }
            result.append({url.toString(QUrl::RemoveUserInfo), name, url, protocol, origin});
        }
    }
    *sources = result;
    return true;
}
} // namespace
bool readVideoConnections(const QByteArray &xml, const QString &origin, QList<VideoSource> *sources)
{
    const auto json = QJsonDocument::fromJson(xml);
    if (json.isObject()) {
        return readVideoJson(json.object(), origin, sources);
    }
    QXmlStreamReader reader(xml);
    QList<VideoSource> result;
    QHash<QString, QString> feed;
    bool inFeed = false;
    int feeds = 0;
    QString field;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
            return false;
        }
        if (token == QXmlStreamReader::StartElement) {
            if (reader.name() == u"feed") {
                if (++feeds > MaxFeeds) {
                    return false;
                }
                inFeed = true;
                feed.clear();
            } else if (inFeed) {
                field = reader.name().toString();
            }
        } else if (token == QXmlStreamReader::Characters && inFeed && !field.isEmpty()) {
            feed[field] += reader.text().toString().left(2048);
        } else if (token == QXmlStreamReader::EndElement) {
            if (reader.name() == u"feed") {
                inFeed = false;
                auto text = feed.value(QStringLiteral("url")).trimmed();
                if (text.isEmpty() && !feed.value(QStringLiteral("address")).trimmed().isEmpty()) {
                    text = feed.value(QStringLiteral("protocol")).trimmed() +
                           QStringLiteral("://") + feed.value(QStringLiteral("address")).trimmed();
                    const auto port = feed.value(QStringLiteral("port")).trimmed();
                    if (!port.isEmpty() && port != u"-1") {
                        text += QLatin1Char(':') + port;
                    }
                    const auto path = feed.value(QStringLiteral("path")).trimmed();
                    if (!path.isEmpty()) {
                        text += path.startsWith(QLatin1Char('/')) ? path : QLatin1Char('/') + path;
                    }
                }
                const QUrl url(text, QUrl::StrictMode);
                const auto protocol = videoProtocol(url);
                if (protocol != VideoProtocol::Unsupported && result.size() < MaxVideoSources) {
                    auto name = plain(feed.value(QStringLiteral("alias")));
                    if (name.isEmpty()) {
                        name =
                            url.host().isEmpty() ? url.toString(QUrl::RemoveUserInfo) : url.host();
                    }
                    result.append(
                        {url.toString(QUrl::RemoveUserInfo), name, url, protocol, origin});
                }
            }
            field.clear();
        }
    }
    if (reader.hasError()) {
        return false;
    }
    *sources = result;
    return true;
}

VideoSources::VideoSources(Store &store, Servers *servers, TakServerApi *api, QObject *parent)
    : QObject(parent), servers_(servers), api_(api)
{
    setObjectName(QStringLiteral("videoSources"));
    connect(&store, &Store::eventAccepted, this, &VideoSources::receive);
}
void VideoSources::receive(const Event &event)
{
    const auto text = videoUrl(event);
    if (text.isEmpty()) {
        return;
    }
    const auto name = plain(event.callsign.isEmpty() ? event.uid : event.callsign);
    add(name, QUrl(text, QUrl::StrictMode), event.origin.isEmpty() ? event.uid : event.origin);
}
QString VideoSources::add(const QString &name, const QUrl &url, const QString &origin)
{
    QString error;
    const auto protocol = videoProtocol(url, &error);
    if (protocol == VideoProtocol::Unsupported) {
        return error;
    }
    const auto key = url.toString(QUrl::RemoveUserInfo);
    const auto title = plain(name).isEmpty() ? key.left(128) : plain(name);
    const auto found = std::find_if(sources_.begin(), sources_.end(), [&key](const auto &source) {
        return source.key == key;
    });
    const bool manual = origin.isEmpty();
    if (found != sources_.end()) {
        // Events and server lists do not change a source that the user entered, so that a peer
        // cannot replace its URL or credentials.
        if (!manual && found->origin.isEmpty()) {
            return {};
        }
        if (found->name != title || found->url != url) {
            found->name = title;
            found->url = url;
            emit changed();
        }
        return {};
    }
    if (sources_.size() >= MaxVideoSources) {
        // Sources from events and servers come back with the next event or refresh. The oldest
        // of them makes room, so that remote events cannot fill the list for the session.
        const auto oldest = std::find_if(sources_.begin(), sources_.end(), [](const auto &item) {
            return !item.origin.isEmpty();
        });
        if (oldest == sources_.end()) {
            return tr("The video list holds at most %1 sources.").arg(MaxVideoSources);
        }
        sources_.erase(oldest);
    }
    sources_.append({key, title, url, protocol, plain(origin)});
    emit changed();
    return {};
}
void VideoSources::remove(const QString &key)
{
    const auto before = sources_.size();
    sources_.removeIf([&key](const auto &source) {
        return source.key == key;
    });
    if (sources_.size() != before) {
        emit changed();
    }
}
void VideoSources::refresh()
{
    if (!servers_ || !api_) {
        emit refreshed(tr("No TAK REST API is available."));
        return;
    }
    auto pending = std::make_shared<int>(0);
    auto errors = std::make_shared<QStringList>();
    const auto entries = servers_->sources();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const auto source = it.value().toObject();
        if (!source.value(QStringLiteral("enabled")).toBool() ||
            TakServerApi::apiUrl(source, QStringLiteral("/video")).isEmpty()) {
            continue;
        }
        ++*pending;
        const auto id = it.key();
        const auto name = source.value(QStringLiteral("name")).toString();
        api_->request(
            id,
            "GET",
            QStringLiteral("/video"),
            {},
            this,
            [this, id, name, pending, errors](const NetworkReply &reply) {
                QList<VideoSource> list;
                if (reply.ok() && readVideoConnections(reply.body, id, &list)) {
                    for (const auto &stream : std::as_const(list)) {
                        add(stream.name, stream.url, id);
                    }
                } else {
                    errors->append(tr("%1: the video list is unavailable.").arg(name));
                }
                if (--*pending == 0) {
                    emit refreshed(errors->join(QLatin1Char(' ')));
                }
            },
            MaxListBytes);
    }
    if (*pending == 0) {
        emit refreshed(tr("No enabled TAK Server entry has a REST API port."));
    }
}
} // namespace swak

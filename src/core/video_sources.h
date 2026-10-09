#pragma once
#include "core/event.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QUrl>

namespace swak {
class Servers;
class Store;
class TakServerApi;

// The stream protocols that TAK video uses.
enum class VideoProtocol
{
    Rtsp,
    Rtmp,
    Srt,
    Hls,
    Udp,
    Unsupported
};
// One video stream from a TAK Server list, a video event, or the user.
struct VideoSource
{
    // The URL without credentials, so the list never shows a password.
    QString key;
    QString name;
    QUrl url;
    VideoProtocol protocol = VideoProtocol::Unsupported;
    // The server entry or event that supplied the source. Empty for manual sources.
    QString origin;
};
inline constexpr int MaxVideoSources = 64;

// Returns the protocol of a stream URL, or Unsupported with a reason in error.
VideoProtocol videoProtocol(const QUrl &url, QString *error = nullptr);
QString videoProtocolName(VideoProtocol protocol);
// Reads a /Marti/api/video reply. TAK Server 5 answers JSON with videoConnections and their
// feeds. Older servers answer videoConnections XML. Returns false for other content.
bool readVideoConnections(const QByteArray &xml,
                          const QString &origin,
                          QList<VideoSource> *sources);

// The video streams of one window.
//
// Video events (b-i-v, or any event with a __video element) add or update a source. Refresh
// reads the video connection lists of the TAK Server entries with a REST API. The list keeps
// at most MaxVideoSources sources and rejects other URL schemes, such as file.
class VideoSources final : public QObject
{
    Q_OBJECT
public:
    VideoSources(Store &store, Servers *servers, TakServerApi *api, QObject *parent = nullptr);
    const QList<VideoSource> &sources() const
    {
        return sources_;
    }
    // Adds a source. Returns an error for a URL that SWAK cannot play.
    QString add(const QString &name, const QUrl &url, const QString &origin = {});
    void remove(const QString &key);
    // Reads the video lists of all enabled TAK Server entries. refreshed reports the result.
    void refresh();
signals:
    void changed();
    void refreshed(const QString &error);

private:
    void receive(const Event &event);
    QPointer<Servers> servers_;
    QPointer<TakServerApi> api_;
    QList<VideoSource> sources_;
};
} // namespace swak

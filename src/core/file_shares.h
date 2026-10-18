#pragma once
#include "core/event.h"
#include "core/package_import.h"
#include "core/tak_shapes.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointer>

namespace swak {
struct PreparedPackage;
class DataSources;
class Identity;
class MapSources;
class Servers;
class Store;
class TakServerApi;
class UserObjects;

// A file that a TAK user offered (b-f-t-r) or a file in the server file list.
struct FileOffer
{
    enum class State
    {
        Offered,
        Downloading,
        Imported,
        Failed
    };
    // The lower-case SHA-256 hash, which also identifies the file on TAK Server.
    QString hash;
    QString serverId;
    QString senderUid;
    QString senderCallsign;
    QString name;
    QString fileName;
    qint64 size = -1;
    QDateTime time;
    State state = State::Offered;
    QString status;
};
// One file from the TAK Server file list (/Marti/sync/search).
struct ServerFile
{
    QString hash;
    QString name;
    QString fileName;
    QString submitter;
    QString mimeType;
    QStringList keywords;
    qint64 size = -1;
    QDateTime time;
};

// Reads a /Marti/sync/search reply. Returns false for other content.
bool readServerFiles(const QByteArray &json, QList<ServerFile> *files);
// Builds the b-f-t-r event that offers an uploaded package to the recipients.
Event fileOfferEvent(const Event &sender,
                     const QString &url,
                     const QString &name,
                     const QString &fileName,
                     const QString &hash,
                     qint64 size,
                     const QList<ShareRecipient> &recipients,
                     const QDateTime &now);

// Data package transfers with TAK Server for one window.
//
// SWAK downloads a file only after the user asks for it, only from the TAK REST API of the
// server entry that delivered the offer, and only by its SHA-256 hash. It checks the size
// and the hash before it imports the package.
class FileShares final : public QObject
{
    Q_OBJECT
public:
    static constexpr int MaxOffers = 64;
    static constexpr int MaxServerFiles = 1000;
    static constexpr qsizetype MaxSendBytes = 32 * 1024 * 1024;
    FileShares(Store &store,
               Servers *servers,
               TakServerApi *api,
               Identity *identity,
               UserObjects *objects,
               DataSources *data,
               MapSources *maps,
               QObject *parent = nullptr);
    const QList<FileOffer> &offers() const
    {
        return offers_;
    }
    // Starts a download and import. Returns an error when it cannot start.
    QString download(const QString &serverId, const QString &hash);
    // Adds a server file as an offer and starts its download.
    QString downloadServerFile(const QString &serverId, const ServerFile &file);
    void dismiss(const QString &serverId, const QString &hash);
    // Reads the server file list. searched reports the result.
    void search(const QString &serverId);
    // Deletes a server file. TAK Server answers with an error when the user cannot delete it.
    void remove(const QString &serverId, const QString &hash);
    // Builds a package from local objects, uploads it, and offers it to the recipients.
    QString send(const QString &serverId,
                 const QString &name,
                 const QStringList &objectIds,
                 const QList<ShareRecipient> &recipients);
    // The folder for imported files. Tests can redirect it.
    void setDirectory(const QString &directory);
    // Receives one b-f-t-r event. The Store calls it.
    void receive(const Event &event);
signals:
    void changed();
    void
    searched(const QString &serverId, const QList<swak::ServerFile> &files, const QString &error);
    void removed(const QString &serverId, const QString &hash, const QString &error);
    void sent(const QString &error);

private:
    FileOffer *find(const QString &serverId, const QString &hash);
    void finish(const QString &serverId, const QString &hash, const QByteArray &bytes);
    void
    importPrepared(const QString &serverId, const QString &hash, const PreparedPackage &prepared);
    void fail(const QString &serverId, const QString &hash, const QString &error);
    Event sender() const;
    QPointer<Servers> servers_;
    QPointer<TakServerApi> api_;
    QPointer<Identity> identity_;
    QPointer<UserObjects> objects_;
    QPointer<DataSources> data_;
    QPointer<MapSources> maps_;
    QList<FileOffer> offers_;
    QString directory_;
    bool sending_ = false;
};
} // namespace swak
Q_DECLARE_METATYPE(swak::ServerFile)

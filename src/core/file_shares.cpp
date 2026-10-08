#include "core/file_shares.h"

#include "core/cot.h"
#include "core/cot_detail.h"
#include "core/data.h"
#include "core/identity.h"
#include "core/logging.h"
#include "core/maps.h"
#include "core/object_sharing.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/tak_server_api.h"
#include "core/user_objects.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace swak {
namespace {
constexpr int TransferTimeoutMs = 600000;
constexpr qsizetype MaxSearchBytes = 4 * 1024 * 1024;
constexpr int OfferLifetimeSeconds = 24 * 60 * 60;

bool hashText(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9a-fA-F]{64}\\z"));
    return pattern.match(text).hasMatch();
}
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
// A file name without folders or characters that file systems or HTTP headers reject.
QString safeFileName(const QString &text)
{
    QString result;
    for (const auto ch : plain(text, 100)) {
        result += ch.isLetterOrNumber() || ch == u'.' || ch == u'-' || ch == u'_' || ch == u' '
                      ? ch
                      : QChar(u'_');
    }
    while (result.startsWith(u'.')) {
        result.remove(0, 1);
    }
    return result.isEmpty() ? QStringLiteral("package") : result;
}
QString encoded(const QString &value)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}
QString httpError(const NetworkReply &reply)
{
    if (!reply.error.isEmpty()) {
        return reply.error;
    }
    if (reply.status == 401 || reply.status == 403) {
        return QCoreApplication::translate("FileShares",
                                           "The server does not allow this for your user.");
    }
    if (reply.status == 404) {
        return QCoreApplication::translate("FileShares", "The server does not have this file.");
    }
    return QCoreApplication::translate("FileShares", "The server answered with HTTP %1.")
        .arg(reply.status);
}
} // namespace

bool readServerFiles(const QByteArray &json, QList<ServerFile> *files)
{
    const auto document = QJsonDocument::fromJson(json);
    const auto results = document.object().value(QStringLiteral("results"));
    if (!document.isObject() || !results.isArray() ||
        results.toArray().size() > FileShares::MaxServerFiles) {
        return false;
    }
    QList<ServerFile> next;
    for (const auto &value : results.toArray()) {
        const auto object = value.toObject();
        ServerFile file;
        file.hash = object.value(QStringLiteral("hash")).toString().toLower();
        if (!hashText(file.hash)) {
            continue;
        }
        file.name = plain(object.value(QStringLiteral("name")).toString());
        file.fileName = plain(object.value(QStringLiteral("filename")).toString());
        file.submitter = plain(object.value(QStringLiteral("submitter")).toString());
        file.mimeType = plain(object.value(QStringLiteral("mimeType")).toString(), 100);
        for (const auto &keyword : object.value(QStringLiteral("keywords")).toArray()) {
            if (file.keywords.size() < 16) {
                file.keywords.append(plain(keyword.toString(), 64));
            }
        }
        const double size = object.value(QStringLiteral("size")).toDouble(-1);
        file.size = size >= 0 && size < 1e15 ? static_cast<qint64>(size) : -1;
        file.time = QDateTime::fromString(object.value(QStringLiteral("submissionTime")).toString(),
                                          Qt::ISODateWithMs);
        if (file.name.isEmpty()) {
            file.name = file.fileName.isEmpty() ? file.hash.left(12) : file.fileName;
        }
        next.append(file);
    }
    *files = next;
    return true;
}
Event fileOfferEvent(const Event &sender,
                     const QString &url,
                     const QString &name,
                     const QString &fileName,
                     const QString &hash,
                     qint64 size,
                     const QList<ShareRecipient> &recipients,
                     const QDateTime &now)
{
    Event event;
    event.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    event.type = QStringLiteral("b-f-t-r");
    event.how = QStringLiteral("h-e");
    event.time = event.start = now;
    event.stale = now.addSecs(OfferLifetimeSeconds);
    event.altitude = 9999999;
    std::vector<CotElement> detail{
        {QStringLiteral("fileshare"),
         {{QStringLiteral("filename"), fileName},
          {QStringLiteral("name"), name},
          {QStringLiteral("senderUrl"), url},
          {QStringLiteral("sizeInBytes"), QString::number(size)},
          {QStringLiteral("sha256"), hash},
          {QStringLiteral("senderUid"), sender.uid},
          {QStringLiteral("senderCallsign"), sender.callsign}},
         {},
         {}},
        {QStringLiteral("ackrequest"),
         {{QStringLiteral("uid"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
          {QStringLiteral("ackrequested"), QStringLiteral("true")},
          {QStringLiteral("tag"), name}},
         {},
         {}}};
    if (!recipients.isEmpty()) {
        CotElement marti{QStringLiteral("marti"), {}, {}, {}};
        for (const auto &recipient : recipients) {
            marti.children.push_back(
                {QStringLiteral("dest"),
                 {recipient.callsign.isEmpty()
                      ? std::pair{QStringLiteral("uid"), recipient.uid}
                      : std::pair{QStringLiteral("callsign"), recipient.callsign}},
                 {},
                 {}});
        }
        detail.push_back(std::move(marti));
    }
    event.detail = detailText(detail);
    return event;
}

FileShares::FileShares(Store &store,
                       Servers *servers,
                       TakServerApi *api,
                       Identity *identity,
                       UserObjects *objects,
                       DataSources *data,
                       MapSources *maps,
                       QObject *parent)
    : QObject(parent), servers_(servers), api_(api), identity_(identity), objects_(objects),
      data_(data), maps_(maps),
      directory_(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                 QStringLiteral("/packages"))
{
    setObjectName(QStringLiteral("fileShares"));
    qRegisterMetaType<swak::ServerFile>();
    connect(&store, &Store::messageReceived, this, &FileShares::receive);
}
void FileShares::setDirectory(const QString &directory)
{
    directory_ = directory;
}
FileOffer *FileShares::find(const QString &serverId, const QString &hash)
{
    const auto found = std::find_if(offers_.begin(), offers_.end(), [&](const FileOffer &offer) {
        return offer.serverId == serverId && offer.hash == hash.toLower();
    });
    return found == offers_.end() ? nullptr : &*found;
}
void FileShares::receive(const Event &event)
{
    if (event.type != QStringLiteral("b-f-t-r") || event.origin.isEmpty()) {
        return;
    }
    const auto share = fileShare(event);
    if (!share || !hashText(share->sha256)) {
        return;
    }
    FileOffer offer;
    offer.hash = share->sha256.toLower();
    offer.serverId = event.origin;
    offer.senderUid = plain(share->senderUid);
    offer.senderCallsign = plain(share->senderCallsign);
    if (offer.senderCallsign.isEmpty()) {
        offer.senderCallsign = offer.senderUid;
    }
    offer.fileName = plain(share->fileName);
    offer.name = plain(share->name);
    if (offer.name.isEmpty()) {
        offer.name = offer.fileName.isEmpty() ? offer.hash.left(12) : offer.fileName;
    }
    offer.size = share->sizeInBytes;
    offer.time = event.time;
    const auto source =
        servers_ ? servers_->sources().value(offer.serverId).toObject() : QJsonObject{};
    if (offer.size > MaxPackageBytes) {
        offer.state = FileOffer::State::Failed;
        offer.status = tr("The file is larger than 64 MiB.");
    } else if (TakServerApi::martiUrl(source, QStringLiteral("/sync/content")).isEmpty()) {
        offer.state = FileOffer::State::Failed;
        offer.status = tr("Only TAK Server entries with a REST API can deliver files.");
    }
    if (auto *existing = find(offer.serverId, offer.hash)) {
        // A repeated offer updates the sender but keeps the download state.
        existing->senderUid = offer.senderUid;
        existing->senderCallsign = offer.senderCallsign;
        existing->name = offer.name;
        existing->time = offer.time;
    } else {
        if (offers_.size() >= MaxOffers) {
            const auto oldest = std::find_if(offers_.begin(), offers_.end(), [](const auto &item) {
                return item.state != FileOffer::State::Downloading;
            });
            if (oldest == offers_.end()) {
                return;
            }
            offers_.erase(oldest);
        }
        offers_.append(offer);
        logMessage(Severity::Info,
                   Urgency::Attention,
                   QStringLiteral("swak.files"),
                   tr("%1 offers the data package %2.").arg(offer.senderCallsign, offer.name));
    }
    emit changed();
}
QString FileShares::download(const QString &serverId, const QString &hash)
{
    auto *offer = find(serverId, hash);
    if (!offer) {
        return tr("The file offer does not exist.");
    }
    if (offer->state == FileOffer::State::Downloading) {
        return tr("The file is downloading.");
    }
    if (offer->size > MaxPackageBytes) {
        return tr("The file is larger than 64 MiB.");
    }
    if (!api_) {
        return tr("No TAK REST API is available.");
    }
    const auto key = offer->hash;
    const auto id = api_->martiRequest(
        serverId,
        "GET",
        QStringLiteral("/sync/content?hash=") + key,
        {},
        {},
        this,
        [this, serverId, key](const NetworkReply &reply) {
            if (!reply.ok()) {
                fail(serverId, key, httpError(reply));
                return;
            }
            finish(serverId, key, reply.body);
        },
        MaxPackageBytes,
        TransferTimeoutMs);
    // A rejected request answers through the callback, so the state changes there too.
    Q_UNUSED(id)
    offer = find(serverId, key);
    if (offer && offer->state != FileOffer::State::Failed) {
        offer->state = FileOffer::State::Downloading;
        offer->status = tr("Downloading…");
    }
    emit changed();
    return {};
}
QString FileShares::downloadServerFile(const QString &serverId, const ServerFile &file)
{
    if (!hashText(file.hash)) {
        return tr("The server file has no valid hash.");
    }
    if (!find(serverId, file.hash)) {
        if (offers_.size() >= MaxOffers) {
            const auto oldest = std::find_if(offers_.begin(), offers_.end(), [](const auto &item) {
                return item.state != FileOffer::State::Downloading;
            });
            if (oldest == offers_.end()) {
                return tr("Too many files are downloading.");
            }
            offers_.erase(oldest);
        }
        FileOffer offer;
        offer.hash = file.hash.toLower();
        offer.serverId = serverId;
        offer.senderCallsign = file.submitter;
        offer.name = file.name;
        offer.fileName = file.fileName;
        offer.size = file.size;
        offer.time = file.time.isValid() ? file.time : QDateTime::currentDateTimeUtc();
        offers_.append(offer);
    }
    return download(serverId, file.hash);
}
void FileShares::dismiss(const QString &serverId, const QString &hash)
{
    const auto before = offers_.size();
    offers_.removeIf([&](const FileOffer &offer) {
        return offer.serverId == serverId && offer.hash == hash.toLower() &&
               offer.state != FileOffer::State::Downloading;
    });
    if (offers_.size() != before) {
        emit changed();
    }
}
void FileShares::fail(const QString &serverId, const QString &hash, const QString &error)
{
    if (auto *offer = find(serverId, hash)) {
        offer->state = FileOffer::State::Failed;
        offer->status = error;
        emit changed();
    }
}
void FileShares::finish(const QString &serverId, const QString &hash, const QByteArray &bytes)
{
    auto *offer = find(serverId, hash);
    if (!offer) {
        return;
    }
    if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) !=
        hash) {
        fail(serverId, hash, tr("The file does not match its SHA-256 hash."));
        return;
    }
    auto package = readDataPackage(bytes);
    if (!package.error.isEmpty()) {
        // TAK clients also share single files, such as a KML file.
        const auto fileName = QFileInfo(offer->fileName).fileName();
        if (!safePackagePath(fileName) || bytes.startsWith("PK")) {
            fail(serverId, hash, package.error);
            return;
        }
        package = {};
        package.entries.append({fileName, bytes});
    }
    if (package.name.isEmpty()) {
        package.name = offer->name;
    }
    const auto title = offer->name;
    const auto result = importDataPackage(
        package, hash, directory_ + QLatin1Char('/') + hash, objects_, data_, maps_);
    // The import can change other services and their signals. Find the offer again.
    offer = find(serverId, hash);
    if (!offer) {
        return;
    }
    offer->state = result.errors.isEmpty() ? FileOffer::State::Imported : FileOffer::State::Failed;
    offer->status = result.summary();
    logMessage(Severity::Info,
               Urgency::Routine,
               QStringLiteral("swak.files"),
               tr("Data package %1: %2").arg(title, offer->status));
    emit changed();
}
void FileShares::search(const QString &serverId)
{
    if (!api_) {
        emit searched(serverId, {}, tr("No TAK REST API is available."));
        return;
    }
    api_->martiRequest(
        serverId,
        "GET",
        QStringLiteral("/sync/search"),
        {},
        {},
        this,
        [this, serverId](const NetworkReply &reply) {
            QList<ServerFile> files;
            if (!reply.ok()) {
                emit searched(serverId, {}, httpError(reply));
            } else if (!readServerFiles(reply.body, &files)) {
                emit searched(serverId, {}, tr("The server file list is invalid."));
            } else {
                emit searched(serverId, files, {});
            }
        },
        MaxSearchBytes,
        60000);
}
void FileShares::remove(const QString &serverId, const QString &hash)
{
    if (!api_ || !hashText(hash)) {
        emit removed(serverId, hash, tr("The file cannot be deleted."));
        return;
    }
    api_->martiRequest(
        serverId,
        "DELETE",
        QStringLiteral("/sync/delete?hash=") + hash.toLower(),
        {},
        {},
        this,
        [this, serverId, hash](const NetworkReply &reply) {
            emit removed(serverId, hash, reply.ok() ? QString{} : httpError(reply));
        },
        64 * 1024,
        60000);
}
QString FileShares::send(const QString &serverId,
                         const QString &name,
                         const QStringList &objectIds,
                         const QList<ShareRecipient> &recipients)
{
    if (sending_) {
        return tr("Another package is uploading.");
    }
    if (!api_ || !servers_ || !objects_) {
        return tr("No TAK REST API is available.");
    }
    const auto source = servers_->sources().value(serverId).toObject();
    if (TakServerApi::martiUrl(source, QStringLiteral("/sync/content")).isEmpty()) {
        return tr("Choose a TAK Server entry with a REST API.");
    }
    if (recipients.isEmpty() || recipients.size() > ObjectSharing::MaxRecipients) {
        return tr("Choose 1 to %1 recipients.").arg(ObjectSharing::MaxRecipients);
    }
    const auto title = plain(name);
    if (title.isEmpty()) {
        return tr("Type a package name.");
    }
    const auto from = sender();
    const auto now = QDateTime::currentDateTimeUtc();
    QList<PackageEntry> entries;
    for (const auto &id : objectIds) {
        const auto item = objects_->find(id);
        const auto event = item ? sharedObjectEvent(*item, from, {}, now) : std::nullopt;
        if (!event) {
            return tr("An object cannot be sent as TAK CoT.");
        }
        entries.append({QStringLiteral("%1/%1.cot").arg(item->id), encodeCot(*event)});
    }
    if (entries.isEmpty()) {
        return tr("Choose at least one object.");
    }
    QString error;
    const auto uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto zip = writeDataPackage(uid, title, entries, &error);
    if (zip.isEmpty()) {
        return error;
    }
    if (zip.size() > MaxSendBytes) {
        return tr("The package is larger than 32 MiB.");
    }
    const auto hash =
        QString::fromLatin1(QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex());
    const auto fileName = safeFileName(title) + QStringLiteral(".zip");
    const auto boundary = QUuid::createUuid().toString(QUuid::Id128).toLatin1();
    QByteArray body =
        "--" + boundary + "\r\nContent-Disposition: form-data; name=\"assetfile\"; filename=\"" +
        fileName.toUtf8() + "\"\r\nContent-Type: application/x-zip-compressed\r\n\r\n" + zip +
        "\r\n--" + boundary + "--\r\n";
    const auto url =
        TakServerApi::martiUrl(source, QStringLiteral("/sync/content?hash=") + hash).toString();
    sending_ = true;
    api_->martiRequest(
        serverId,
        "POST",
        QStringLiteral("/sync/missionupload?hash=%1&filename=%2&creatorUid=%3")
            .arg(hash, encoded(fileName), encoded(from.uid)),
        body,
        "multipart/form-data; boundary=" + boundary,
        this,
        [this, from, url, title, fileName, hash, size = zip.size(), recipients](
            const NetworkReply &reply) {
            sending_ = false;
            if (!reply.ok()) {
                emit sent(httpError(reply));
                return;
            }
            const auto offer = fileOfferEvent(from,
                                              url,
                                              title,
                                              fileName,
                                              hash,
                                              size,
                                              recipients,
                                              QDateTime::currentDateTimeUtc());
            emit sent(servers_ ? servers_->send(offer) : tr("No output is connected."));
        },
        64 * 1024,
        TransferTimeoutMs);
    return {};
}
Event FileShares::sender() const
{
    Event event;
    if (identity_) {
        identity_->apply(event);
    }
    return event;
}
} // namespace swak

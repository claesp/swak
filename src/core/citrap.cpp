#include "core/citrap.h"

#include "core/identity.h"
#include "core/tak_server_api.h"

#include <QHash>
#include <QRegularExpression>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cmath>

namespace swak {
namespace {
constexpr int RequestTimeoutMs = 120000;

QString plain(const QString &text, qsizetype limit = 256)
{
    QString result;
    for (const auto ch : text.left(limit)) {
        if (ch == u'\n' || (ch.category() != QChar::Other_Control &&
                            ch.category() != QChar::Other_Format && !ch.isSurrogate())) {
            result += ch;
        }
    }
    return result.trimmed();
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
        return Citrap::tr("The server does not allow this for your user.");
    }
    return Citrap::tr("The server answered with HTTP %1.").arg(reply.status);
}
// A file name without folders or characters that HTTP headers reject.
QString safeFileName(const QString &text)
{
    QString result;
    for (const auto ch : plain(text, 100)) {
        result +=
            ch.isLetterOrNumber() || ch == u'.' || ch == u'-' || ch == u'_' ? ch : QChar(u'_');
    }
    while (result.startsWith(u'.')) {
        result.remove(0, 1);
    }
    return result.isEmpty() ? QStringLiteral("attachment") : result;
}
CitrapReport reportFrom(const QHash<QString, QString> &fields)
{
    CitrapReport report;
    const auto field = [&fields](std::initializer_list<const char *> names, qsizetype limit = 256) {
        for (const auto *name : names) {
            const auto value = fields.value(QLatin1String(name));
            if (!value.isEmpty()) {
                return plain(value, limit);
            }
        }
        return QString{};
    };
    report.id = field({"id", "uid"}, 128);
    report.title = field({"title"}, 128);
    report.type = field({"type"}, 64);
    report.status = field({"status"}, 64);
    report.importance = field({"importance"}, 64);
    report.callsign = field({"userCallsign", "callsign"}, 128);
    report.description = field({"userDescription", "description"}, 4096);
    report.time = QDateTime::fromString(field({"dateTime", "time"}, 64), Qt::ISODateWithMs);
    const auto location = field({"location"}, 64).split(QLatin1Char(','));
    if (location.size() >= 2) {
        bool latitudeOk = false;
        bool longitudeOk = false;
        const double latitude = location.at(0).trimmed().toDouble(&latitudeOk);
        const double longitude = location.at(1).trimmed().toDouble(&longitudeOk);
        if (latitudeOk && longitudeOk && std::abs(latitude) <= 90 && std::abs(longitude) <= 180) {
            report.latitude = latitude;
            report.longitude = longitude;
        }
    }
    return report;
}
} // namespace

bool validCitrapId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9._-]{1,128}\\z"));
    return pattern.match(id).hasMatch() && id != u"." && id != u"..";
}
bool readCitrapReports(const QByteArray &xml, QList<CitrapReport> *reports)
{
    QXmlStreamReader reader(xml);
    QList<CitrapReport> result;
    QHash<QString, QString> fields;
    int depth = 0;
    int reportDepth = -1;
    QString field;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
            return false;
        }
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            if (reader.name() == u"report" && reportDepth < 0) {
                if (result.size() >= MaxCitrapReports) {
                    return false;
                }
                reportDepth = depth;
                fields.clear();
                for (const auto &attribute : reader.attributes()) {
                    fields.insert(attribute.name().toString(),
                                  attribute.value().toString().left(4096));
                }
            } else if (reportDepth >= 0 && depth == reportDepth + 1) {
                field = reader.name().toString();
            }
        } else if (token == QXmlStreamReader::Characters && reportDepth >= 0 &&
                   depth == reportDepth + 1 && !field.isEmpty()) {
            auto &value = fields[field];
            if (value.size() < 4096) {
                value += reader.text().toString().left(4096 - value.size());
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (depth == reportDepth) {
                reportDepth = -1;
                const auto report = reportFrom(fields);
                if (validCitrapId(report.id)) {
                    result.append(report);
                }
            }
            field.clear();
            --depth;
        }
    }
    if (reader.hasError()) {
        return false;
    }
    *reports = result;
    return true;
}
QByteArray citrapReportXml(const CitrapReport &report)
{
    QByteArray output;
    QXmlStreamWriter writer(&output);
    writer.writeStartDocument();
    writer.writeStartElement(QStringLiteral("report"));
    const auto attribute = [&writer](const QString &name, const QString &value) {
        if (!value.isEmpty()) {
            writer.writeAttribute(name, plain(value));
        }
    };
    attribute(QStringLiteral("id"), report.id);
    attribute(QStringLiteral("title"), report.title);
    attribute(QStringLiteral("type"), report.type);
    attribute(QStringLiteral("status"), report.status);
    attribute(QStringLiteral("importance"), report.importance);
    attribute(QStringLiteral("userCallsign"), report.callsign);
    attribute(QStringLiteral("dateTime"),
              (report.time.isValid() ? report.time : QDateTime::currentDateTimeUtc())
                  .toUTC()
                  .toString(Qt::ISODateWithMs));
    attribute(QStringLiteral("location"),
              QStringLiteral("%1,%2").arg(QString::number(report.latitude, 'f', 7),
                                          QString::number(report.longitude, 'f', 7)));
    writer.writeTextElement(QStringLiteral("userDescription"), plain(report.description, 4096));
    writer.writeEndElement();
    writer.writeEndDocument();
    return output;
}

Citrap::Citrap(TakServerApi *api, Identity *identity, QObject *parent)
    : QObject(parent), api_(api), identity_(identity)
{
    setObjectName(QStringLiteral("citrap"));
    qRegisterMetaType<swak::CitrapReport>();
}
QString Citrap::clientUid() const
{
    Event event;
    if (identity_) {
        identity_->apply(event);
    }
    return event.uid;
}
void Citrap::list(const QString &serverId)
{
    if (!api_) {
        emit listed(serverId, {}, tr("No TAK REST API is available."));
        return;
    }
    api_->martiRequest(
        serverId,
        "GET",
        QStringLiteral("/api/citrap?maxReportCount=%1").arg(MaxCitrapReports),
        {},
        {},
        this,
        [this, serverId](const NetworkReply &reply) {
            QList<CitrapReport> reports;
            if (!reply.ok()) {
                emit listed(serverId, {}, httpError(reply));
            } else if (!readCitrapReports(reply.body, &reports)) {
                emit listed(serverId, {}, tr("The report list is invalid."));
            } else {
                emit listed(serverId, reports, {});
            }
        },
        MaxReplyBytes,
        RequestTimeoutMs);
}
QString Citrap::create(const QString &serverId,
                       CitrapReport report,
                       const QList<CitrapAttachment> &attachments)
{
    if (!api_) {
        return tr("No TAK REST API is available.");
    }
    if (plain(report.title, 128).isEmpty()) {
        return tr("Type a report title.");
    }
    if (attachments.size() > MaxCitrapAttachments) {
        return tr("Add at most %1 attachments.").arg(MaxCitrapAttachments);
    }
    if (std::any_of(attachments.cbegin(), attachments.cend(), [](const CitrapAttachment &item) {
            return item.bytes.isEmpty() || item.bytes.size() > MaxCitrapAttachmentBytes;
        })) {
        return tr("Each attachment must contain 1 byte to 16 MiB.");
    }
    if (!validCitrapId(report.id)) {
        report.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (report.status.isEmpty()) {
        report.status = QStringLiteral("Open");
    }
    api_->martiRequest(
        serverId,
        "POST",
        QStringLiteral("/api/citrap?clientUid=") + encoded(clientUid()),
        citrapReportXml(report),
        "application/xml",
        this,
        [this, serverId, id = report.id, attachments](const NetworkReply &reply) {
            if (!reply.ok()) {
                emit created(serverId, {}, httpError(reply));
                return;
            }
            // The server answers with the stored report or its identifier.
            QList<CitrapReport> stored;
            auto created = id;
            if (readCitrapReports(reply.body, &stored) && !stored.isEmpty()) {
                created = stored.first().id;
            } else if (validCitrapId(QString::fromUtf8(reply.body).trimmed())) {
                created = QString::fromUtf8(reply.body).trimmed();
            }
            upload(serverId, created, attachments, {});
        },
        64 * 1024,
        RequestTimeoutMs);
    return {};
}
void Citrap::upload(const QString &serverId,
                    const QString &id,
                    QList<CitrapAttachment> attachments,
                    QStringList errors)
{
    if (attachments.isEmpty() || !api_) {
        emit created(serverId, id, errors.join(QLatin1Char(' ')));
        return;
    }
    const auto attachment = attachments.takeFirst();
    const auto fileName = safeFileName(attachment.fileName);
    const auto boundary = QUuid::createUuid().toString(QUuid::Id128).toLatin1();
    const QByteArray body =
        "--" + boundary + "\r\nContent-Disposition: form-data; name=\"assetfile\"; filename=\"" +
        fileName.toUtf8() + "\"\r\nContent-Type: application/octet-stream\r\n\r\n" +
        attachment.bytes + "\r\n--" + boundary + "--\r\n";
    api_->martiRequest(
        serverId,
        "POST",
        QStringLiteral("/api/citrap/%1/attachment?clientUid=%2&filename=%3")
            .arg(id, encoded(clientUid()), encoded(fileName)),
        body,
        "multipart/form-data; boundary=" + boundary,
        this,
        [this, serverId, id, attachments, errors, fileName](const NetworkReply &reply) mutable {
            if (!reply.ok()) {
                errors.append(tr("%1: %2").arg(fileName, httpError(reply)));
            }
            upload(serverId, id, attachments, errors);
        },
        64 * 1024,
        RequestTimeoutMs);
}
QString Citrap::update(const QString &serverId, const CitrapReport &report)
{
    if (!api_) {
        return tr("No TAK REST API is available.");
    }
    if (!validCitrapId(report.id)) {
        return tr("The report cannot be updated.");
    }
    api_->martiRequest(
        serverId,
        "PUT",
        QStringLiteral("/api/citrap/%1?clientUid=%2").arg(report.id, encoded(clientUid())),
        citrapReportXml(report),
        "application/xml",
        this,
        [this, serverId, id = report.id](const NetworkReply &reply) {
            emit updated(serverId, id, reply.ok() ? QString{} : httpError(reply));
        },
        64 * 1024,
        RequestTimeoutMs);
    return {};
}
} // namespace swak

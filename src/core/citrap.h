#pragma once
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

namespace swak {
class Identity;
class TakServerApi;

// One CI-TRAP report from TAK Server (/Marti/api/citrap).
struct CitrapReport
{
    QString id;
    QString title;
    QString type;
    QString status;
    QString importance;
    QString callsign;
    QString description;
    QDateTime time;
    double latitude = 0;
    double longitude = 0;
};
// One file for a report.
struct CitrapAttachment
{
    QString fileName;
    QByteArray bytes;
};
inline constexpr int MaxCitrapReports = 500;
inline constexpr int MaxCitrapAttachments = 8;
inline constexpr qsizetype MaxCitrapAttachmentBytes = 16 * 1024 * 1024;

// Reads a report list. It accepts report fields as attributes or as child elements.
// Returns false for other content.
bool readCitrapReports(const QByteArray &xml, QList<CitrapReport> *reports);
// Writes one report as XML for a create or update request.
QByteArray citrapReportXml(const CitrapReport &report);

// The CI-TRAP reports of TAK Server entries for one window.
class Citrap final : public QObject
{
    Q_OBJECT
public:
    static constexpr qsizetype MaxReplyBytes = 4 * 1024 * 1024;
    Citrap(TakServerApi *api, Identity *identity, QObject *parent = nullptr);
    // Reads the newest reports of a server. listed reports the result.
    void list(const QString &serverId);
    // Creates a report and then uploads its attachments. created reports the result.
    QString create(const QString &serverId,
                   CitrapReport report,
                   const QList<CitrapAttachment> &attachments);
    // Sends a changed report, for example a new status. updated reports the result.
    QString update(const QString &serverId, const CitrapReport &report);
signals:
    void
    listed(const QString &serverId, const QList<swak::CitrapReport> &reports, const QString &error);
    void created(const QString &serverId, const QString &id, const QString &error);
    void updated(const QString &serverId, const QString &id, const QString &error);

private:
    void upload(const QString &serverId,
                const QString &id,
                QList<CitrapAttachment> attachments,
                QStringList errors);
    QString clientUid() const;
    QPointer<TakServerApi> api_;
    QPointer<Identity> identity_;
};
// True for a report identifier that SWAK can put in a URL path.
bool validCitrapId(const QString &id);
} // namespace swak
Q_DECLARE_METATYPE(swak::CitrapReport)

#pragma once
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

namespace swak {
class Identity;
class TakServerApi;

// One task of a TAK ExCheck checklist.
struct ChecklistTask
{
    QString uid;
    // The cell values in column order.
    QStringList values;
    QString status;
    // The task as received, so an update keeps the fields that SWAK does not show.
    QByteArray xml;
    bool complete() const
    {
        return status.startsWith(QStringLiteral("COMPLETE"));
    }
};
// A TAK ExCheck checklist. Active checklists are missions on TAK Server.
struct Checklist
{
    QString uid;
    QString name;
    QString description;
    QString templateName;
    QDateTime started;
    QStringList columns;
    QList<ChecklistTask> tasks;
};
// A checklist template from the exchecktemplates mission.
struct ChecklistTemplate
{
    QString uid;
    QString name;
};
inline constexpr int MaxChecklists = 256;
inline constexpr int MaxChecklistTasks = 500;
inline constexpr int MaxChecklistColumns = 32;

// Reads a checklist or a list of checklists (/excheck/checklist/active). Returns false for
// other content. List replies give checklists without tasks when the server omits them.
bool readChecklists(const QByteArray &xml, QList<Checklist> *checklists);
// Reads the templates of the exchecktemplates mission reply.
QList<ChecklistTemplate> readChecklistTemplates(const QByteArray &json);
// Returns the task XML with a new status. Complete statuses get completeDTG, and other
// statuses remove it. Returns an empty array for invalid task XML.
QByteArray taskWithStatus(const QByteArray &taskXml, const QString &status, const QDateTime &now);

// The ExCheck checklists of TAK Server entries for one window.
class ExCheck final : public QObject
{
    Q_OBJECT
public:
    static constexpr qsizetype MaxReplyBytes = 4 * 1024 * 1024;
    ExCheck(TakServerApi *api, Identity *identity, QObject *parent = nullptr);
    // Reads the active checklists and the templates of a server. listed reports the result.
    void list(const QString &serverId);
    // Reads one checklist with its tasks. loaded reports the result.
    void load(const QString &serverId, const QString &uid);
    // Starts a checklist from a template. started reports the result.
    QString start(const QString &serverId,
                  const QString &templateUid,
                  const QString &name,
                  const QString &description);
    // Sets the status of one task. updated reports the result.
    QString setStatus(const QString &serverId,
                      const QString &checklistUid,
                      const ChecklistTask &task,
                      const QString &status);
signals:
    void listed(const QString &serverId,
                const QList<swak::Checklist> &checklists,
                const QList<swak::ChecklistTemplate> &templates,
                const QString &error);
    void loaded(const QString &serverId, const swak::Checklist &checklist, const QString &error);
    void started(const QString &serverId, const QString &error);
    void updated(const QString &serverId, const QString &checklistUid, const QString &error);

private:
    QString clientUid(QString *callsign = nullptr) const;
    QPointer<TakServerApi> api_;
    QPointer<Identity> identity_;
};
// True for a checklist, task, or template identifier that SWAK can put in a URL path.
bool validChecklistId(const QString &id);
} // namespace swak
Q_DECLARE_METATYPE(swak::Checklist)
Q_DECLARE_METATYPE(swak::ChecklistTemplate)

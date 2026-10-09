#include "core/excheck.h"

#include "core/identity.h"
#include "core/tak_server_api.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <memory>

namespace swak {
namespace {
constexpr int RequestTimeoutMs = 60000;

QString plain(const QString &text, qsizetype limit = 512)
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
        return ExCheck::tr("The server does not allow this for your user.");
    }
    return ExCheck::tr("The server answered with HTTP %1.").arg(reply.status);
}
// These task elements come after completeDTG in the schema sequence.
bool afterCompleteTime(QStringView name)
{
    return name == u"notes" || name == u"dueRelativeTime" || name == u"dueDTG" ||
           name == u"lineBreakText";
}
// TAK Server accepts only these XML values for checklistTaskStatus.
bool validStatus(const QString &status)
{
    return status == u"Pending" || status == u"Complete" || status == u"Complete (late)" ||
           status == u"Late";
}
// These task elements come after status in the schema sequence.
bool afterStatus(QStringView name)
{
    return name == u"customStatus" || name == u"completeBy" || afterCompleteTime(name);
}
} // namespace

bool validChecklistId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9._-]{1,128}\\z"));
    return pattern.match(id).hasMatch() && id != u"." && id != u"..";
}
bool readChecklists(const QByteArray &xml, QList<Checklist> *checklists)
{
    // Offsets into the decoded text give the raw XML of each task.
    const auto text = QString::fromUtf8(xml);
    QXmlStreamReader reader(text);
    QList<Checklist> result;
    Checklist current;
    ChecklistTask task;
    bool inChecklist = false;
    bool inDetails = false;
    bool inColumn = false;
    bool inTask = false;
    qsizetype taskStart = -1;
    QString value;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
            return false;
        }
        if (token == QXmlStreamReader::StartElement) {
            const auto name = reader.name();
            value.clear();
            if (name == u"checklist") {
                if (result.size() >= MaxChecklists) {
                    return false;
                }
                inChecklist = true;
                current = {};
            } else if (inChecklist && name == u"checklistDetails") {
                inDetails = true;
            } else if (inChecklist && name == u"checklistColumn") {
                inColumn = true;
            } else if (inChecklist && name == u"checklistTask") {
                if (current.tasks.size() >= MaxChecklistTasks) {
                    return false;
                }
                inTask = true;
                task = {};
                taskStart =
                    text.lastIndexOf(QStringLiteral("<checklistTask"), reader.characterOffset());
            }
        } else if (token == QXmlStreamReader::Characters) {
            value += reader.text();
        } else if (token == QXmlStreamReader::EndElement) {
            const auto name = reader.name();
            if (inTask) {
                if (name == u"uid") {
                    task.uid = plain(value, 128);
                } else if (name == u"value" && task.values.size() < MaxChecklistColumns) {
                    task.values.append(plain(value));
                } else if (name == u"status") {
                    task.status = plain(value, 32);
                } else if (name == u"checklistTask") {
                    inTask = false;
                    if (taskStart >= 0) {
                        task.xml =
                            text.mid(taskStart, reader.characterOffset() - taskStart).toUtf8();
                    }
                    if (validChecklistId(task.uid)) {
                        current.tasks.append(task);
                    }
                }
            } else if (inColumn) {
                if (name == u"columnName" && current.columns.size() < MaxChecklistColumns) {
                    current.columns.append(plain(value, 128));
                } else if (name == u"checklistColumn") {
                    inColumn = false;
                }
            } else if (inDetails) {
                if (name == u"uid") {
                    current.uid = plain(value, 128);
                } else if (name == u"name") {
                    current.name = plain(value, 128);
                } else if (name == u"description") {
                    current.description = plain(value);
                } else if (name == u"templateName") {
                    current.templateName = plain(value, 128);
                } else if (name == u"startTime") {
                    current.started = QDateTime::fromString(value.trimmed(), Qt::ISODateWithMs);
                } else if (name == u"checklistDetails") {
                    inDetails = false;
                }
            } else if (inChecklist && name == u"checklist") {
                inChecklist = false;
                if (validChecklistId(current.uid)) {
                    if (current.name.isEmpty()) {
                        current.name = current.uid;
                    }
                    result.append(current);
                }
            }
            value.clear();
        }
    }
    if (reader.hasError()) {
        return false;
    }
    *checklists = result;
    return true;
}
QList<ChecklistTemplate> readChecklistTemplates(const QByteArray &json)
{
    QList<ChecklistTemplate> result;
    const auto data = QJsonDocument::fromJson(json).object().value(QStringLiteral("data"));
    const auto mission = data.isArray() ? data.toArray().first().toObject() : data.toObject();
    for (const auto &value : mission.value(QStringLiteral("contents")).toArray()) {
        if (result.size() >= MaxChecklists) {
            break;
        }
        const auto item = value.toObject().value(QStringLiteral("data")).toObject();
        ChecklistTemplate entry{item.value(QStringLiteral("uid")).toString(),
                                plain(item.value(QStringLiteral("name")).toString(), 128)};
        if (validChecklistId(entry.uid)) {
            if (entry.name.isEmpty()) {
                entry.name = entry.uid;
            }
            result.append(entry);
        }
    }
    return result;
}
QByteArray taskWithStatus(const QByteArray &taskXml, const QString &status, const QDateTime &now)
{
    if (!validStatus(status) || taskXml.size() > 64 * 1024) {
        return {};
    }
    const bool complete = status.startsWith(QStringLiteral("Complete"));
    QXmlStreamReader reader(taskXml);
    QByteArray output;
    QXmlStreamWriter writer(&output);
    int depth = 0;
    int skipping = 0;
    bool statusWritten = false;
    bool timeWritten = !complete;
    bool root = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
            return {};
        }
        if (skipping > 0) {
            if (token == QXmlStreamReader::StartElement) {
                ++skipping;
            } else if (token == QXmlStreamReader::EndElement && --skipping == 0) {
                --depth;
            }
            continue;
        }
        switch (token) {
        case QXmlStreamReader::StartElement:
            ++depth;
            if (depth == 1) {
                if (reader.name() != u"checklistTask") {
                    return {};
                }
                root = true;
            } else if (depth == 2 && reader.name() == u"status") {
                writer.writeTextElement(QStringLiteral("status"), status);
                statusWritten = true;
                skipping = 1;
                continue;
            } else if (depth == 2 && reader.name() == u"completeDTG") {
                skipping = 1;
                continue;
            }
            if (depth == 2 && !statusWritten && afterStatus(reader.name())) {
                writer.writeTextElement(QStringLiteral("status"), status);
                statusWritten = true;
            }
            if (depth == 2 && !timeWritten && afterCompleteTime(reader.name())) {
                writer.writeTextElement(QStringLiteral("completeDTG"),
                                        now.toUTC().toString(Qt::ISODateWithMs));
                timeWritten = true;
            }
            writer.writeStartElement(reader.namespaceUri().toString(), reader.name().toString());
            writer.writeAttributes(reader.attributes());
            break;
        case QXmlStreamReader::EndElement:
            if (depth == 1) {
                if (!statusWritten) {
                    writer.writeTextElement(QStringLiteral("status"), status);
                }
                if (!timeWritten) {
                    writer.writeTextElement(QStringLiteral("completeDTG"),
                                            now.toUTC().toString(Qt::ISODateWithMs));
                }
            }
            writer.writeEndElement();
            --depth;
            break;
        case QXmlStreamReader::Characters:
            if (depth > 0) {
                writer.writeCharacters(reader.text().toString());
            }
            break;
        default:
            break;
        }
    }
    if (reader.hasError() || !root) {
        return {};
    }
    return output;
}

ExCheck::ExCheck(TakServerApi *api, Identity *identity, QObject *parent)
    : QObject(parent), api_(api), identity_(identity)
{
    setObjectName(QStringLiteral("excheck"));
    qRegisterMetaType<swak::Checklist>();
    qRegisterMetaType<swak::ChecklistTemplate>();
}
QString ExCheck::clientUid(QString *callsign) const
{
    Event event;
    if (identity_) {
        identity_->apply(event);
    }
    if (callsign) {
        *callsign = event.callsign;
    }
    return event.uid;
}
void ExCheck::list(const QString &serverId)
{
    if (!api_) {
        emit listed(serverId, {}, {}, tr("No TAK REST API is available."));
        return;
    }
    struct State
    {
        int pending = 2;
        QList<Checklist> checklists;
        QList<ChecklistTemplate> templates;
        QString error;
    };
    auto state = std::make_shared<State>();
    const auto finish = [this, serverId, state] {
        if (--state->pending == 0) {
            emit listed(serverId, state->checklists, state->templates, state->error);
        }
    };
    api_->martiRequest(
        serverId,
        "GET",
        QStringLiteral("/api/excheck/checklist/active?clientUid=") + encoded(clientUid()),
        {},
        {},
        this,
        [state, finish](const NetworkReply &reply) {
            if (!reply.ok()) {
                state->error = httpError(reply);
            } else if (!readChecklists(reply.body, &state->checklists)) {
                state->error = tr("The checklist list is invalid.");
            }
            finish();
        },
        MaxReplyBytes,
        RequestTimeoutMs);
    api_->martiRequest(
        serverId,
        "GET",
        QStringLiteral("/api/missions/exchecktemplates"),
        {},
        {},
        this,
        [state, finish](const NetworkReply &reply) {
            // A server without templates answers 404. The checklists still show.
            if (reply.ok()) {
                state->templates = readChecklistTemplates(reply.body);
            }
            finish();
        },
        MaxReplyBytes,
        RequestTimeoutMs);
}
void ExCheck::load(const QString &serverId, const QString &uid)
{
    if (!api_ || !validChecklistId(uid)) {
        emit loaded(serverId, {}, tr("The checklist cannot be read."));
        return;
    }
    api_->martiRequest(
        serverId,
        "GET",
        QStringLiteral("/api/excheck/checklist/%1?clientUid=%2").arg(uid, encoded(clientUid())),
        {},
        {},
        this,
        [this, serverId](const NetworkReply &reply) {
            QList<Checklist> list;
            if (!reply.ok()) {
                emit loaded(serverId, {}, httpError(reply));
            } else if (!readChecklists(reply.body, &list) || list.isEmpty()) {
                emit loaded(serverId, {}, tr("The checklist is invalid."));
            } else {
                emit loaded(serverId, list.first(), {});
            }
        },
        MaxReplyBytes,
        RequestTimeoutMs);
}
QString ExCheck::start(const QString &serverId,
                       const QString &templateUid,
                       const QString &name,
                       const QString &description)
{
    const auto title = plain(name, 128);
    if (!api_) {
        return tr("No TAK REST API is available.");
    }
    if (!validChecklistId(templateUid) || title.isEmpty()) {
        return tr("Choose a template and type a name.");
    }
    QString callsign;
    const auto uid = clientUid(&callsign);
    api_->martiRequest(
        serverId,
        "POST",
        // TAK Server requires all of these parameters, including the start time.
        QStringLiteral("/api/excheck/%1/start?name=%2&description=%3&callsign=%4&clientUid=%5"
                       "&startTime=%6")
            .arg(templateUid,
                 encoded(title),
                 encoded(plain(description)),
                 encoded(callsign),
                 encoded(uid),
                 encoded(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs))),
        {},
        {},
        this,
        [this, serverId](const NetworkReply &reply) {
            emit started(serverId, reply.ok() ? QString{} : httpError(reply));
        },
        MaxReplyBytes,
        RequestTimeoutMs);
    return {};
}
QString ExCheck::setStatus(const QString &serverId,
                           const QString &checklistUid,
                           const ChecklistTask &task,
                           const QString &status)
{
    if (!api_) {
        return tr("No TAK REST API is available.");
    }
    if (!validChecklistId(checklistUid) || !validChecklistId(task.uid)) {
        return tr("The task cannot be updated.");
    }
    const auto body = taskWithStatus(task.xml, status, QDateTime::currentDateTimeUtc());
    if (body.isEmpty()) {
        return tr("The task cannot be updated.");
    }
    api_->martiRequest(
        serverId,
        "PUT",
        QStringLiteral("/api/excheck/checklist/%1/task/%2?clientUid=%3")
            .arg(checklistUid, task.uid, encoded(clientUid())),
        body,
        "application/xml",
        this,
        [this, serverId, checklistUid](const NetworkReply &reply) {
            emit updated(serverId, checklistUid, reply.ok() ? QString{} : httpError(reply));
        },
        64 * 1024,
        RequestTimeoutMs);
    return {};
}
} // namespace swak

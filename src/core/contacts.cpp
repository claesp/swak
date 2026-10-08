#include "core/contacts.h"

#include "core/cot_detail.h"
#include "core/servers.h"
#include "core/store.h"
#include "core/tak_server_api.h"

#include <QCollator>
#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <memory>

namespace swak {
namespace {
QString bounded(const QString &text)
{
    QString result;
    for (const auto ch : text.left(128)) {
        if (ch.category() != QChar::Other_Control && ch.category() != QChar::Other_Format) {
            result += ch;
        }
    }
    return result;
}
} // namespace
bool readTakContacts(const QByteArray &json, const QString &origin, QList<Contact> *contacts)
{
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isArray() || document.array().size() > Contacts::MaxContacts) {
        return false;
    }
    QList<Contact> result;
    for (const auto &value : document.array()) {
        const auto object = value.toObject();
        Contact contact;
        contact.uid = bounded(object.value(QStringLiteral("uid")).toString());
        contact.callsign = bounded(object.value(QStringLiteral("callsign")).toString());
        contact.team = bounded(object.value(QStringLiteral("team")).toString());
        contact.role = bounded(object.value(QStringLiteral("role")).toString());
        contact.device = bounded(object.value(QStringLiteral("takv")).toString());
        contact.origin = origin;
        if (contact.uid.isEmpty()) {
            continue;
        }
        result.append(contact);
    }
    *contacts = result;
    return true;
}
Contacts::Contacts(Store &store, Servers *servers, TakServerApi *api, QObject *parent)
    : QAbstractTableModel(parent), store_(store), servers_(servers), api_(api)
{
    setObjectName(QStringLiteral("contacts"));
    timer_.setSingleShot(true);
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &Contacts::rebuild);
    connect(&store_, &Store::changed, this, [this] {
        if (!timer_.isActive()) {
            timer_.start();
        }
    });
    rebuild();
}
int Contacts::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(contacts_.size());
}
int Contacts::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}
QVariant Contacts::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= contacts_.size()) {
        return {};
    }
    const auto &contact = contacts_.at(index.row());
    if (role == Qt::UserRole) {
        return contact.uid;
    }
    if (role == Qt::ForegroundRole && !contact.online) {
        return QColor(Qt::gray);
    }
    if (role == Qt::ToolTipRole) {
        return QStringList{contact.uid, contact.endpoint, contact.device}
            .join(QLatin1Char('\n'))
            .trimmed();
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    switch (index.column()) {
    case CallsignColumn:
        return contact.callsign.isEmpty() ? contact.uid : contact.callsign;
    case TeamColumn:
        return contact.team;
    case RoleColumn:
        return contact.role;
    case ServerColumn: {
        const auto name = servers_ ? servers_->sources()
                                         .value(contact.origin)
                                         .toObject()
                                         .value(QStringLiteral("name"))
                                         .toString()
                                   : QString{};
        return name.isEmpty() ? contact.origin : name;
    }
    case ReportColumn:
        return contact.lastReport.isValid() ? contact.lastReport.toLocalTime().toString(
                                                  QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                            : tr("No current position");
    default:
        return {};
    }
}
QVariant Contacts::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
        section >= ColumnCount) {
        return {};
    }
    return QStringList{tr("Callsign"), tr("Team"), tr("Role"), tr("Server"), tr("Last report")}.at(
        section);
}
void Contacts::rebuild()
{
    QHash<QString, Contact> merged;
    // Forget server lists of deleted entries.
    for (auto it = serverContacts_.begin(); it != serverContacts_.end();) {
        if (!servers_ || !servers_->sources().contains(it.key())) {
            it = serverContacts_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = serverContacts_.cbegin(); it != serverContacts_.cend(); ++it) {
        for (const auto &contact : it.value()) {
            merged.insert(contact.uid, contact);
        }
    }
    for (const auto &event : store_.events()) {
        const auto endpoint = contactEndpoint(event);
        // Local events and objects without a contact endpoint are not users.
        if (event.origin.isEmpty() || endpoint.isEmpty() || merged.size() >= MaxContacts) {
            continue;
        }
        auto &contact = merged[event.uid];
        contact.uid = event.uid;
        contact.callsign = event.callsign;
        contact.team = event.team;
        contact.role = event.role;
        contact.endpoint = endpoint;
        contact.origin = event.origin;
        contact.lastReport = event.time;
        contact.online = true;
        if (const auto device = detailElement(event, u"takv")) {
            contact.device =
                QStringList{device->attribute(u"platform"), device->attribute(u"version")}
                    .join(QLatin1Char(' '))
                    .trimmed()
                    .left(128);
        }
    }
    auto next = merged.values();
    const QCollator collator;
    std::sort(next.begin(), next.end(), [&collator](const Contact &a, const Contact &b) {
        const auto left = a.callsign.isEmpty() ? a.uid : a.callsign;
        const auto right = b.callsign.isEmpty() ? b.uid : b.callsign;
        const int order = collator.compare(left, right);
        return order != 0 ? order < 0 : a.uid < b.uid;
    });
    if (next.size() > MaxContacts) {
        next.resize(MaxContacts);
    }
    // A busy stream changes the store often. Keep the view state when no contact changed.
    if (next == contacts_) {
        return;
    }
    beginResetModel();
    contacts_ = std::move(next);
    endResetModel();
}
void Contacts::refreshServers()
{
    if (!servers_ || !api_) {
        emit refreshed(tr("No TAK REST API is available."));
        return;
    }
    const auto sources = servers_->sources();
    auto pending = std::make_shared<int>(0);
    auto errors = std::make_shared<QStringList>();
    for (auto it = sources.begin(); it != sources.end(); ++it) {
        const auto source = it.value().toObject();
        if (TakServerApi::apiUrl(source, QStringLiteral("/")).isEmpty() ||
            !source.value(QStringLiteral("enabled")).toBool()) {
            continue;
        }
        ++*pending;
        const auto id = it.key();
        const auto name = source.value(QStringLiteral("name")).toString();
        api_->request(
            id,
            "GET",
            QStringLiteral("/contacts/all"),
            {},
            this,
            [this, id, name, pending, errors](const NetworkReply &reply) {
                QList<Contact> list;
                if (reply.ok() && readTakContacts(reply.body, id, &list)) {
                    serverContacts_.insert(id, list);
                } else {
                    errors->append(tr("%1: %2").arg(
                        name,
                        reply.error.isEmpty()
                            ? tr("the contact list is unavailable (HTTP %1).").arg(reply.status)
                            : reply.error));
                }
                if (--*pending == 0) {
                    rebuild();
                    emit refreshed(errors->join(QLatin1Char(' ')));
                }
            },
            512 * 1024);
    }
    if (*pending == 0) {
        rebuild();
        emit refreshed({});
    }
}
} // namespace swak

#include "core/object_sharing.h"

#include "core/identity.h"
#include "core/logging.h"
#include "core/servers.h"
#include "core/user_objects.h"

#include <QJsonArray>

#include <algorithm>

namespace swak {
namespace {
bool safeText(const QString &text)
{
    return text.size() <= 128 && std::none_of(text.cbegin(), text.cend(), [](QChar ch) {
               return ch.category() == QChar::Other_Control ||
                      ch.category() == QChar::Other_Format || ch.isSurrogate();
           });
}
// Reads recipients. Returns false for invalid content.
bool readRecipients(const QJsonValue &value, QList<ShareRecipient> *recipients)
{
    if (!value.isUndefined() && !value.isArray()) {
        return false;
    }
    const auto array = value.toArray();
    if (array.size() > ObjectSharing::MaxRecipients) {
        return false;
    }
    QList<ShareRecipient> result;
    for (const auto &entry : array) {
        const auto object = entry.toObject();
        ShareRecipient recipient{object.value(QStringLiteral("uid")).toString(),
                                 object.value(QStringLiteral("callsign")).toString()};
        if (recipient.uid.isEmpty() || !safeText(recipient.uid) || !safeText(recipient.callsign)) {
            return false;
        }
        result.append(recipient);
    }
    *recipients = result;
    return true;
}
QJsonArray recipientsJson(const QList<ShareRecipient> &recipients)
{
    QJsonArray result;
    for (const auto &recipient : recipients) {
        result.append(QJsonObject{{QStringLiteral("uid"), recipient.uid},
                                  {QStringLiteral("callsign"), recipient.callsign}});
    }
    return result;
}
} // namespace

ObjectSharing::ObjectSharing(UserObjects &objects,
                             Servers *servers,
                             Identity *identity,
                             QObject *parent)
    : QObject(parent), objects_(objects), servers_(servers), identity_(identity)
{
    setObjectName(QStringLiteral("objectSharing"));
    retry_.setSingleShot(true);
    retry_.setInterval(RetryMilliseconds);
    connect(&retry_, &QTimer::timeout, this, &ObjectSharing::flush);
    republish_.setInterval(RepublishMilliseconds);
    connect(&republish_, &QTimer::timeout, this, [this] {
        for (auto it = states_.cbegin(); it != states_.cend(); ++it) {
            if (!it->deleted) {
                enqueue(it.key());
            }
        }
        flush();
    });
    republish_.start();
    connect(&objects_, &UserObjects::changed, this, &ObjectSharing::objectsChanged);
    connect(&objects_, &UserObjects::added, this, [this](const QString &id) {
        const auto item = objects_.find(id);
        if (automatic_ && item && !sharedCotType(*item).isEmpty()) {
            share(id);
        }
    });
}
QList<SettingField> ObjectSharing::settingsFields() const
{
    return {booleanSetting(QStringLiteral("automatic"), tr("Share new objects automatically"))};
}
QJsonObject ObjectSharing::savePreferences() const
{
    QJsonArray shared;
    for (auto it = states_.cbegin(); it != states_.cend(); ++it) {
        // A deleted object cannot come back after a restart, because Undo history ends.
        if (!it->deleted) {
            shared.append(
                QJsonObject{{QStringLiteral("id"), it.key()},
                            {QStringLiteral("type"), it->cotType},
                            {QStringLiteral("recipients"), recipientsJson(it->recipients)}});
        }
    }
    return {{QStringLiteral("automatic"), automatic_}, {QStringLiteral("shared"), shared}};
}
QString ObjectSharing::validateSettings(const QJsonObject &values) const
{
    const auto value = values.value(QStringLiteral("automatic"));
    if (!value.isUndefined() && !value.isBool()) {
        return tr("Automatic sharing must be on or off.");
    }
    const auto shared = values.value(QStringLiteral("shared"));
    if (!shared.isUndefined() && (!shared.isArray() || shared.toArray().size() > MaxShared)) {
        return tr("The list of shared objects is invalid.");
    }
    for (const auto &entry : shared.toArray()) {
        const auto object = entry.toObject();
        const auto id = object.value(QStringLiteral("id")).toString();
        QList<ShareRecipient> parsed;
        const auto type = object.value(QStringLiteral("type")).toString();
        if (id.isEmpty() || !safeText(id) || type.size() > 16 ||
            (!type.isEmpty() && !type.startsWith(QStringLiteral("u-d-")) &&
             !type.startsWith(QStringLiteral("b-m-"))) ||
            !readRecipients(object.value(QStringLiteral("recipients")), &parsed)) {
            return tr("The list of shared objects is invalid.");
        }
    }
    return {};
}
void ObjectSharing::restorePreferences(const QJsonObject &values)
{
    if (!validateSettings(values).isEmpty()) {
        return;
    }
    automatic_ = values.value(QStringLiteral("automatic")).toBool(automatic_);
    // The Settings page returns the same list. Keep the send state of known objects.
    if (values.contains(QStringLiteral("shared"))) {
        QHash<QString, State> next;
        for (const auto &entry : values.value(QStringLiteral("shared")).toArray()) {
            const auto object = entry.toObject();
            const auto id = object.value(QStringLiteral("id")).toString();
            auto state = states_.value(id);
            readRecipients(object.value(QStringLiteral("recipients")), &state.recipients);
            if (state.cotType.isEmpty()) {
                state.cotType = object.value(QStringLiteral("type")).toString();
            }
            const bool known = states_.contains(id);
            next.insert(id, state);
            if (!known) {
                enqueue(id);
            }
        }
        states_ = std::move(next);
        pending_.removeIf([this](const QString &id) {
            return !states_.contains(id);
        });
    }
    emit changed();
    // The objects can restore after this service. Send when the event loop runs again.
    if (!pending_.isEmpty()) {
        retry_.start(0);
    }
}
void ObjectSharing::setAutomatic(bool enabled)
{
    if (automatic_ != enabled) {
        automatic_ = enabled;
        emit changed();
    }
}
bool ObjectSharing::isShared(const QString &id) const
{
    const auto it = states_.constFind(id);
    return it != states_.cend() && !it->deleted;
}
QList<ShareRecipient> ObjectSharing::recipients(const QString &id) const
{
    return states_.value(id).recipients;
}
QString ObjectSharing::share(const QString &id, const QList<ShareRecipient> &recipients)
{
    const auto item = objects_.find(id);
    if (!item) {
        return tr("The object does not exist.");
    }
    if (sharedCotType(*item).isEmpty()) {
        return tr("TAK clients cannot show this object type.");
    }
    if (recipients.size() > MaxRecipients) {
        return tr("Choose at most %1 recipients.").arg(MaxRecipients);
    }
    if (!states_.contains(id) && states_.size() >= MaxShared) {
        return tr("At most %1 objects can be shared.").arg(MaxShared);
    }
    auto &state = states_[id];
    // Users that no longer receive the object get a delete.
    QList<ShareRecipient> removed;
    if (!state.recipients.isEmpty()) {
        for (const auto &previous : std::as_const(state.recipients)) {
            if (!recipients.isEmpty() && !recipients.contains(previous)) {
                removed.append(previous);
            }
        }
    }
    if (!removed.isEmpty()) {
        queueDelete(
            sharedDeleteEvent(id, sharedCotType(*item), removed, QDateTime::currentDateTimeUtc()));
    }
    state.recipients = recipients;
    state.cotType = sharedCotType(*item);
    state.deleted = false;
    state.sent = {};
    enqueue(id);
    emit changed();
    flush();
    return {};
}
void ObjectSharing::stopSharing(const QString &id)
{
    const auto it = states_.constFind(id);
    if (it == states_.cend()) {
        return;
    }
    if (!it->deleted) {
        const auto item = objects_.find(id);
        const auto type = item ? sharedCotType(*item) : it->cotType;
        queueDelete(sharedDeleteEvent(id, type, it->recipients, QDateTime::currentDateTimeUtc()));
    }
    states_.erase(it);
    pending_.removeAll(id);
    emit changed();
    flush();
}
void ObjectSharing::objectsChanged()
{
    const auto now = QDateTime::currentDateTimeUtc();
    bool queued = false;
    for (auto it = states_.begin(); it != states_.end(); ++it) {
        const auto item = objects_.find(it.key());
        if (!item) {
            if (!it->deleted && !it->cotType.isEmpty()) {
                queueDelete(sharedDeleteEvent(it.key(), it->cotType, it->recipients, now));
                it->deleted = true;
                it->sent = {};
                pending_.removeAll(it.key());
                queued = true;
            }
            continue;
        }
        it->cotType = sharedCotType(*item);
        if (it->deleted || item->json() != it->sent) {
            it->deleted = false;
            enqueue(it.key());
            queued = true;
        }
    }
    if (queued) {
        emit changed();
        flush();
    }
}
void ObjectSharing::queueDelete(const Event &event)
{
    // Without a connection, deletes wait. Drop the oldest so the queue stays bounded.
    if (deletes_.size() >= 2 * MaxShared) {
        deletes_.removeFirst();
    }
    deletes_.append(event);
}
void ObjectSharing::enqueue(const QString &id)
{
    if (!pending_.contains(id)) {
        pending_.append(id);
    }
}
void ObjectSharing::report(const QString &error)
{
    // Retries repeat the same error. Log each new error once.
    if (error != lastError_) {
        logMessage(Severity::Warning,
                   Urgency::Attention,
                   QStringLiteral("swak.sharing"),
                   tr("Shared objects wait for sending: %1").arg(error));
    }
    lastError_ = error;
}
Event ObjectSharing::sender() const
{
    Event event;
    if (identity_) {
        identity_->apply(event);
    }
    return event;
}
void ObjectSharing::flush()
{
    if (!servers_) {
        return;
    }
    const auto failed = [this](const QString &error) {
        report(error);
        if (!retry_.isActive()) {
            retry_.start(RetryMilliseconds);
        }
    };
    while (!deletes_.isEmpty()) {
        const auto error = servers_->send(deletes_.first());
        if (!error.isEmpty()) {
            failed(error);
            return;
        }
        deletes_.removeFirst();
    }
    const auto from = sender();
    const auto now = QDateTime::currentDateTimeUtc();
    while (!pending_.isEmpty()) {
        const auto id = pending_.first();
        const auto item = objects_.find(id);
        auto state = states_.find(id);
        if (!item || state == states_.end()) {
            pending_.removeFirst();
            continue;
        }
        const auto event = sharedObjectEvent(*item, from, state->recipients, now);
        if (!event) {
            // The object changed to content that TAK cannot show. Try again after a change.
            report(tr("%1 cannot be sent as TAK CoT.").arg(item->title));
            state->sent = item->json();
            pending_.removeFirst();
            continue;
        }
        const auto error = servers_->send(*event);
        if (!error.isEmpty()) {
            failed(error);
            return;
        }
        state->sent = item->json();
        state->cotType = event->type;
        pending_.removeFirst();
    }
    lastError_.clear();
}
} // namespace swak

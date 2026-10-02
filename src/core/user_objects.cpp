#include "core/user_objects.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUndoCommand>
#include <cmath>
#include <algorithm>
#include <numeric>
namespace swak {
namespace {
qsizetype bytes(const UserObject &item) { return QJsonDocument(item.json()).toJson(QJsonDocument::Indented).size(); }
constexpr qsizetype MaxBytes = 128 * 1024;
constexpr int MaxObjects = 256;
bool boundedGeometry(const QJsonValue &value, int depth = 0)
{
    if (depth > 8) return false;
    if (value.isObject()) {
        const auto object = value.toObject(); if (object.size() > 64) return false;
        for (auto it = object.begin(); it != object.end(); ++it) if (it.key().size() > 64 || !boundedGeometry(it.value(), depth + 1)) return false;
    } else if (value.isArray()) {
        const auto array = value.toArray(); if (array.size() > 256) return false;
        return std::all_of(array.cbegin(), array.cend(), [depth](const auto &entry) { return boundedGeometry(entry, depth + 1); });
    } else if (value.isString()) return value.toString().size() <= 256;
    else if (value.isDouble()) return std::isfinite(value.toDouble());
    return true;
}
}
QJsonObject UserObject::json() const
{
    return {{QStringLiteral("id"), id}, {QStringLiteral("type"), type}, {QStringLiteral("title"), title},
        {QStringLiteral("description"), description}, {QStringLiteral("properties"), properties}, {QStringLiteral("geometry"), geometry}};
}
UserObject UserObject::fromJson(const QJsonObject &v)
{
    return {v.value(QStringLiteral("id")).toString(), v.value(QStringLiteral("type")).toString(),
        v.value(QStringLiteral("title")).toString(), v.value(QStringLiteral("description")).toString(),
        v.value(QStringLiteral("properties")).toObject(), v.value(QStringLiteral("geometry")).toObject()};
}
class DeleteObject final : public QUndoCommand {
public:
    DeleteObject(UserObjects &objects, UserObject item) : objects_(objects), item_(std::move(item)), size_(bytes(item_)) { setText(UserObjects::tr("Delete object")); }
    ~DeleteObject() override { if (deleted_) release(); }
    void redo() override
    {
        if (const auto current = objects_.find(item_.id)) { item_ = *current; size_ = bytes(item_); }
        objects_.items_.remove(item_.id);
        objects_.deletedIds_.insert(item_.id);
        objects_.reservedBytes_ += size_; ++objects_.reservedCount_; deleted_ = true;
        emit objects_.changed();
    }
    void undo() override
    {
        release(); deleted_ = false;
        objects_.items_.insert(item_.id, item_);
        emit objects_.changed(); emit objects_.restored(item_.id);
    }
private:
    void release() { objects_.deletedIds_.remove(item_.id); objects_.reservedBytes_ -= size_; --objects_.reservedCount_; }
    UserObjects &objects_;
    UserObject item_;
    qsizetype size_;
    bool deleted_ = false;
};
UserObjects::UserObjects(QObject *parent) : QObject(parent) { undo_.setUndoLimit(100); }
QString UserObjects::validate(const UserObject &item)
{
    static const QRegularExpression identifier(QStringLiteral("\\A[a-zA-Z0-9._-]{1,64}\\z"));
    if (!identifier.match(item.id).hasMatch() || !identifier.match(item.type).hasMatch()) return tr("Invalid object identity or type.");
    if (item.title.trimmed().isEmpty() || item.title.size() > 128 || item.description.size() > 2048) return tr("Use a title up to 128 characters and a description up to 2048 characters.");
    if (item.properties.size() > 32) return tr("Use at most 32 properties.");
    for (auto it = item.properties.begin(); it != item.properties.end(); ++it) {
        if (it.key().trimmed().isEmpty() || it.key().size() > 64 || !it.value().isString() || it.value().toString().size() > 256)
            return tr("Use property names up to 64 characters and text values up to 256 characters.");
    }
    if (item.properties.contains(QStringLiteral("ID")) || item.properties.contains(QStringLiteral("Latitude")) || item.properties.contains(QStringLiteral("Longitude")))
        return tr("ID, Latitude, and Longitude are reserved property names.");
    if (item.geometry.isEmpty() || !boundedGeometry(item.geometry) || bytes(item) > 8192) return tr("The object exceeds the storage limit.");
    if (item.type == QStringLiteral("circle")) {
        for (const auto *key : {"x", "y"}) {
            const auto value = item.geometry.value(QLatin1String(key));
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0 || value.toDouble() > 1) return tr("The marker position is outside the map.");
        }
    }
    return {};
}
std::optional<UserObject> UserObjects::find(const QString &id) const
{
    const auto it = items_.constFind(id);
    return it == items_.cend() ? std::nullopt : std::optional<UserObject>(*it);
}
QString UserObjects::put(const UserObject &item)
{
    const auto error = validate(item); if (!error.isEmpty()) return error;
    if (const auto previous = find(item.id); previous && previous->type != item.type) return tr("An object cannot change its type.");
    if (deletedIds_.contains(item.id)) return tr("This object is deleted. Use Undo before editing it.");
    const auto total = std::accumulate(items_.cbegin(), items_.cend(), bytes(item) + reservedBytes_,
        [&item](qsizetype sum, const UserObject &existing) { return sum + (existing.id == item.id ? 0 : bytes(existing)); });
    if (total > MaxBytes || (!items_.contains(item.id) && items_.size() + reservedCount_ >= MaxObjects))
        return tr("Marker storage is full. Restart after deleting unused markers to release undo history.");
    items_.insert(item.id, item); emit changed(); return {};
}
void UserObjects::remove(const QString &id)
{
    const auto item = find(id); if (item) undo_.push(new DeleteObject(*this, *item));
}
QJsonObject UserObjects::savePreferences() const
{
    QJsonArray records; for (const auto &item : items_) records.append(item.json());
    return {{QStringLiteral("items"), records}};
}
void UserObjects::restorePreferences(const QJsonObject &values)
{
    undo_.clear(); items_.clear();
    const auto records = values.value(QStringLiteral("items")).toArray();
    for (qsizetype i = 0; i < std::min<qsizetype>(records.size(), MaxObjects); ++i) {
        const auto item = UserObject::fromJson(records.at(i).toObject());
        if (items_.contains(item.id)) continue;
        put(item);
    }
    emit changed();
}
}

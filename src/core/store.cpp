#include "core/store.h"

#include "core/cot_detail.h"

#include <QLocale>

#include <algorithm>

namespace swak {
Store::Store(QObject *parent) : QAbstractTableModel(parent), motion_(new TrackMotion(this))
{
    expiry_.setInterval(1000);
    connect(&expiry_, &QTimer::timeout, this, [this] {
        expire();
        if (!events_.isEmpty()) {
            emit changed();
        }
    });
    expiry_.start();
}
int Store::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(events_.size());
}
int Store::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : 5;
}
QVariant Store::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= events_.size() ||
        index.column() >= 5) {
        return {};
    }
    const auto &event = events_.at(index.row());
    if (role == Qt::UserRole) {
        return event.uid;
    }
    if (role == SortRole) {
        if (index.column() == 2) {
            return event.latitude;
        }
        if (index.column() == 3) {
            return event.longitude;
        }
        if (index.column() == 4) {
            return event.stale;
        }
    }
    if (role != Qt::DisplayRole && role != SortRole) {
        return {};
    }
    switch (index.column()) {
    case 0:
        return event.callsign.isEmpty() ? event.uid : event.callsign;
    case 1:
        return event.type;
    case 2:
        return QLocale().toString(event.latitude, 'f', 5);
    case 3:
        return QLocale().toString(event.longitude, 'f', 5);
    case 4:
        return event.stale.toUTC().toString(Qt::ISODate);
    default:
        return {};
    }
}
QVariant Store::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return {};
    }
    const QStringList headers{
        tr("Identity"), tr("Type"), tr("Latitude"), tr("Longitude"), tr("Expires (UTC)")};
    return section >= 0 && section < headers.size() ? headers.at(section) : QVariant{};
}
QString Store::accept(const Event &event, const QDateTime &now)
{
    const auto error = eventError(event);
    if (!error.isEmpty()) {
        return error;
    }
    if (event.stale <= now) {
        return tr("The event expired.");
    }
    if (event.start > now.addSecs(300) || event.time > now.addSecs(300)) {
        return tr("The event time is too far in the future.");
    }
    if (event.stale > now.addDays(7)) {
        return tr("The event lifetime exceeds seven days.");
    }
    if (deleteRequest(event)) {
        return {};
    }
    if (event.type == QLatin1String("b-t-f") || event.type.startsWith(QLatin1String("b-t-f-"))) {
        emit messageReceived(event);
        return {};
    }
    const auto found = rows_.constFind(event.uid);
    const qsizetype previous = found != rows_.cend() ? events_.at(found.value()).detail.size() : 0;
    Event stored = event;
    if (detailCharacters_ - previous + stored.detail.size() > DetailBudget) {
        stored.detail.clear();
        stored.detailTruncated = true;
    }
    if (found != rows_.cend()) {
        const int row = found.value();
        if (event.time < events_.at(row).time) {
            return tr("A newer event already exists for this identity.");
        }
        detailCharacters_ += stored.detail.size() - previous;
        events_[row] = stored;
        if (batching_) {
            firstChanged_ = std::min(firstChanged_, row);
            lastChanged_ = std::max(lastChanged_, row);
        } else {
            emit dataChanged(QAbstractTableModel::index(row, 0),
                             QAbstractTableModel::index(row, 4));
        }
    } else {
        if (events_.size() >= MaxEvents) {
            return tr("The event limit is full. Clear events or wait for them to expire.");
        }
        const int row = rowCount();
        beginInsertRows({}, row, row);
        events_.append(stored);
        rows_.insert(event.uid, row);
        detailCharacters_ += stored.detail.size();
        endInsertRows();
    }
    motion_->update(stored);
    emit eventAccepted(stored);
    if (!batching_) {
        emit changed();
    }
    return {};
}
void Store::acceptBatch(const QList<Event> &events)
{
    batching_ = true;
    firstChanged_ = MaxEvents;
    lastChanged_ = -1;
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &event : events) {
        accept(event, now);
    }
    batching_ = false;
    if (lastChanged_ >= firstChanged_) {
        emit dataChanged(QAbstractTableModel::index(firstChanged_, 0),
                         QAbstractTableModel::index(lastChanged_, 4));
    }
    if (!events.isEmpty()) {
        emit changed();
    }
}
void Store::expire(const QDateTime &now)
{
    const auto selectedRow = rows_.constFind(selected_);
    if (selectedRow != rows_.cend() && events_.at(selectedRow.value()).stale <= now) {
        select({});
    }
    removing_ = true;
    bool removed = false;
    for (int row = rowCount() - 1; row >= 0; --row) {
        if (events_.at(row).stale > now) {
            continue;
        }
        motion_->expire(events_.at(row).uid);
        beginRemoveRows({}, row, row);
        detailCharacters_ -= events_.at(row).detail.size();
        rows_.remove(events_.at(row).uid);
        events_.removeAt(row);
        endRemoveRows();
        removed = true;
    }
    removing_ = false;
    if (!removed) {
        return;
    }
    for (int row = 0; row < rowCount(); ++row) {
        rows_[events_.at(row).uid] = row;
    }
    if (!rows_.contains(selected_)) {
        select({});
    }
    emit changed();
}
// TAK deletes an object with a t-x-d-d event. Its link elements name the objects. SWAK
// removes only objects from the same origin, so one server cannot delete another's objects.
bool Store::deleteRequest(const Event &event)
{
    // An emergency cancel replaces the emergency with the same UID. SWAK removes the alert and
    // keeps no cancel marker.
    if (event.type == QLatin1String("b-a-o-can")) {
        const auto row = rows_.constFind(event.uid);
        if (row != rows_.cend() && events_.at(row.value()).origin == event.origin) {
            removeEvents({event.uid});
        }
        return true;
    }
    if (event.type != QLatin1String("t-x-d-d")) {
        return false;
    }
    QStringList targets;
    for (const auto &link : links(event)) {
        const auto row = rows_.constFind(link.uid);
        if (row != rows_.cend() && events_.at(row.value()).origin == event.origin) {
            targets.append(link.uid);
        }
    }
    removeEvents(targets);
    return true;
}
void Store::removeEvents(const QStringList &uids)
{
    if (uids.isEmpty()) {
        return;
    }
    if (uids.contains(selected_)) {
        select({});
    }
    removing_ = true;
    for (int row = rowCount() - 1; row >= 0; --row) {
        if (!uids.contains(events_.at(row).uid)) {
            continue;
        }
        motion_->remove(events_.at(row).uid);
        beginRemoveRows({}, row, row);
        detailCharacters_ -= events_.at(row).detail.size();
        rows_.remove(events_.at(row).uid);
        events_.removeAt(row);
        endRemoveRows();
    }
    removing_ = false;
    for (int row = 0; row < rowCount(); ++row) {
        rows_[events_.at(row).uid] = row;
    }
    if (!batching_) {
        emit changed();
    }
}
void Store::clear()
{
    motion_->clear();
    beginResetModel();
    events_.clear();
    rows_.clear();
    detailCharacters_ = 0;
    endResetModel();
    select({});
    emit changed();
}
void Store::select(const QString &uid)
{
    if (removing_) {
        return;
    }
    const QString next = rows_.contains(uid) ? uid : QString{};
    if (selected_ == next) {
        return;
    }
    selected_ = next;
    emit selectionChanged();
}
} // namespace swak

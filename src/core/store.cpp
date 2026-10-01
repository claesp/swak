#include "core/store.h"
#include <QLocale>

namespace swak {
Store::Store(QObject *parent) : QAbstractTableModel(parent)
{
    expiry_.setInterval(1000);
    connect(&expiry_, &QTimer::timeout, this, [this] {
        expire();
        if (!events_.isEmpty()) emit changed();
    });
    expiry_.start();
}
int Store::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : static_cast<int>(events_.size()); }
int Store::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : 5; }
QVariant Store::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= events_.size() || index.column() >= 5) return {};
    const auto &event = events_.at(index.row());
    if (role == Qt::UserRole) return event.uid;
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case 0: return event.callsign.isEmpty() ? event.uid : event.callsign;
    case 1: return event.type;
    case 2: return QLocale().toString(event.latitude, 'f', 5);
    case 3: return QLocale().toString(event.longitude, 'f', 5);
    case 4: return event.stale.toUTC().toString(Qt::ISODate);
    default: return {};
    }
}
QVariant Store::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
    const QStringList headers{tr("Identity"), tr("Type"), tr("Latitude"), tr("Longitude"), tr("Expires (UTC)")};
    return section >= 0 && section < headers.size() ? headers.at(section) : QVariant{};
}
QString Store::accept(const Event &event, const QDateTime &now)
{
    const auto error = eventError(event);
    if (!error.isEmpty()) return error;
    if (event.stale <= now) return tr("The event expired.");
    if (event.start > now.addSecs(300) || event.time > now.addSecs(300)) return tr("The event time is too far in the future.");
    if (event.stale > now.addDays(7)) return tr("The event lifetime exceeds seven days.");
    const auto found = rows_.constFind(event.uid);
    if (found != rows_.cend()) {
        const int row = found.value();
        if (event.time < events_.at(row).time) return tr("A newer event already exists for this identity.");
        events_[row] = event;
        emit dataChanged(QAbstractTableModel::index(row, 0), QAbstractTableModel::index(row, 4));
    } else {
        if (events_.size() >= MaxEvents) return tr("The event limit is full. Clear events or wait for them to expire.");
        const int row = rowCount();
        beginInsertRows({}, row, row);
        events_.append(event);
        rows_.insert(event.uid, row);
        endInsertRows();
    }
    emit changed();
    return {};
}
void Store::expire(const QDateTime &now)
{
    bool removed = false;
    for (int row = rowCount() - 1; row >= 0; --row) {
        if (events_.at(row).stale > now) continue;
        beginRemoveRows({}, row, row);
        rows_.remove(events_.at(row).uid);
        events_.removeAt(row);
        endRemoveRows();
        removed = true;
    }
    if (!removed) return;
    for (int row = 0; row < rowCount(); ++row) rows_[events_.at(row).uid] = row;
    if (!rows_.contains(selected_)) select({});
    emit changed();
}
void Store::clear()
{
    beginResetModel();
    events_.clear();
    rows_.clear();
    endResetModel();
    select({});
    emit changed();
}
void Store::select(const QString &uid)
{
    const QString next = rows_.contains(uid) ? uid : QString{};
    if (selected_ == next) return;
    selected_ = next;
    emit selectionChanged();
}
}

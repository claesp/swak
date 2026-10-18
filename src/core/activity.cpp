#include "core/activity.h"

#include "core/preferences.h"

#include <QColor>
#include <QGlobalStatic>

#include <algorithm>
#include <deque>
#include <mutex>

namespace swak {
namespace {
struct Inbox
{
    std::mutex mutex;
    std::deque<ActivityEvent> events;
    quint64 sequence = 0;
};
Q_GLOBAL_STATIC(Inbox, inbox)
QString boundedText(const QString &text, qsizetype limit)
{
    QString result;
    for (const char32_t ch : text.left(limit).toUcs4()) {
        if (QChar::isPrint(ch) && QChar::category(ch) != QChar::Other_Format) {
            result += QString::fromUcs4(&ch, 1);
        }
    }
    return result;
}
} // namespace
void publishActivity(Severity severity,
                     Urgency urgency,
                     const QString &category,
                     const QString &message)
{
    if (inbox.isDestroyed()) {
        return;
    }
    const int level = static_cast<int>(severity);
    const int priority = static_cast<int>(urgency);
    if (level < 0 || level > 4 || priority < 0 || priority > 2) {
        return;
    }
    const std::lock_guard lock(inbox->mutex);
    if (inbox->events.size() == 256) {
        inbox->events.pop_front();
    }
    inbox->events.push_back({++inbox->sequence,
                             QDateTime::currentDateTimeUtc(),
                             severity,
                             urgency,
                             boundedText(category, 128),
                             boundedText(message, 2048)});
}
ActivityModel::ActivityModel(QObject *parent) : QAbstractTableModel(parent)
{
    setObjectName(QStringLiteral("activityModel"));
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, &ActivityModel::collect);
    timer_.start();
}
int ActivityModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(events_.size());
}
int ActivityModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}
QVariant ActivityModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= events_.size() ||
        index.column() < 0 || index.column() >= ColumnCount) {
        return {};
    }
    const auto &event = events_.at(index.row());
    if (role == Qt::ForegroundRole && event.severity >= Severity::Error) {
        return QColor(QStringLiteral("#e53935"));
    }
    if (role == SortRole) {
        if (index.column() == TimeColumn) {
            return event.time;
        }
        if (index.column() == SeverityColumn) {
            return static_cast<int>(event.severity);
        }
    }
    if (role == Qt::ToolTipRole && index.column() == MessageColumn) {
        // Rich text lets the tooltip wrap a long message instead of one wide line.
        return QStringLiteral("<p style=\"white-space: pre-wrap\">%1</p>")
            .arg(event.message.toHtmlEscaped());
    }
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole && role != SortRole) {
        return {};
    }
    switch (index.column()) {
    case TimeColumn:
        return event.time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    case SeverityColumn:
        return QStringList{tr("Debug"), tr("Information"), tr("Warning"), tr("Error"), tr("Fatal")}
            .at(static_cast<int>(event.severity));
    case SourceColumn:
        return event.category;
    case MessageColumn:
        return event.message;
    default:
        return {};
    }
}
QVariant ActivityModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
        section >= ColumnCount) {
        return {};
    }
    return QStringList{tr("Time"), tr("Severity"), tr("Source"), tr("Event")}.at(section);
}
QString ActivityModel::preferenceId() const
{
    return QStringLiteral("org.swak.activity");
}
QString ActivityModel::settingsTitle() const
{
    return tr("Events");
}
QList<SettingField> ActivityModel::settingsFields() const
{
    return {numberSetting(QStringLiteral("capacity"), tr("Events to retain"), 1, 10000)};
}
QJsonObject ActivityModel::savePreferences() const
{
    return {{QStringLiteral("capacity"), capacity_}};
}
void ActivityModel::restorePreferences(const QJsonObject &values)
{
    capacity_ = preferenceInteger(values, QStringLiteral("capacity"), 500, 1, 10000);
    if (events_.size() > capacity_) {
        const int count = static_cast<int>(events_.size()) - capacity_;
        beginRemoveRows({}, 0, count - 1);
        events_.remove(0, count);
        endRemoveRows();
    }
}
void ActivityModel::clear()
{
    {
        const std::lock_guard lock(inbox->mutex);
        sequence_ = inbox->sequence;
    }
    beginResetModel();
    events_.clear();
    endResetModel();
}
void ActivityModel::collect()
{
    QList<ActivityEvent> received;
    {
        const std::lock_guard lock(inbox->mutex);
        if (sequence_ == inbox->sequence) {
            return;
        }
        for (const auto &event : inbox->events) {
            if (event.sequence > sequence_) {
                received.append(event);
            }
        }
        sequence_ = inbox->sequence;
    }
    if (received.isEmpty()) {
        return;
    }
    if (received.size() > capacity_) {
        received.remove(0, received.size() - capacity_);
    }
    const int removeCount =
        std::max(0, static_cast<int>(events_.size() + received.size()) - capacity_);
    if (removeCount) {
        beginRemoveRows({}, 0, removeCount - 1);
        events_.remove(0, removeCount);
        endRemoveRows();
    }
    beginInsertRows({}, rowCount(), rowCount() + static_cast<int>(received.size()) - 1);
    events_.append(received);
    endInsertRows();
}
} // namespace swak

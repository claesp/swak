#pragma once

#include "core/event.h"
#include "core/track_motion.h"
#include <QAbstractTableModel>
#include <QHash>
#include <QTimer>

namespace swak {
class Store final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum { SortRole = Qt::UserRole + 1 };
    explicit Store(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QString accept(const Event &event, const QDateTime &now = QDateTime::currentDateTimeUtc());
    void acceptBatch(const QList<Event> &events);
    void expire(const QDateTime &now = QDateTime::currentDateTimeUtc());
    void clear();
    const TrackMotion &motion() const { return *motion_; }
    const QList<Event> &events() const { return events_; }
    void select(const QString &uid);
    // A value remains valid after the selection changes. QString shares its storage.
    // cppcheck-suppress returnByReference
    QString selected() const { return selected_; }
signals:
    void eventAccepted(const swak::Event &event);
    void changed();
    void selectionChanged();
private:
    TrackMotion *motion_;
    QList<Event> events_;
    QHash<QString, int> rows_;
    QString selected_;
    QTimer expiry_;
    bool removing_ = false;
    bool batching_ = false;
    int firstChanged_ = MaxEvents;
    int lastChanged_ = -1;
};
}

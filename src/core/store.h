#pragma once

#include "core/event.h"
#include <QAbstractTableModel>
#include <QHash>
#include <QTimer>

namespace swak {
class Store final : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit Store(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QString accept(const Event &event, const QDateTime &now = QDateTime::currentDateTimeUtc());
    void expire(const QDateTime &now = QDateTime::currentDateTimeUtc());
    void clear();
    const QList<Event> &events() const { return events_; }
    void select(const QString &uid);
    QString selected() const { return selected_; }
signals:
    void changed();
    void selectionChanged();
private:
    QList<Event> events_;
    QHash<QString, int> rows_;
    QString selected_;
    QTimer expiry_;
};
}

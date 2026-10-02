#pragma once
#include "core/logging.h"
#include "core/settings.h"
#include <QAbstractTableModel>
#include <QDateTime>
#include <QTimer>

namespace swak {
struct ActivityEvent {
    quint64 sequence;
    QDateTime time;
    Severity severity;
    Urgency urgency;
    QString category;
    QString message;
};
void publishActivity(Severity severity, Urgency urgency, const QString &category, const QString &message);
class ActivityModel final : public QAbstractTableModel, public SettingsParticipant {
    Q_OBJECT
public:
    enum { SortRole = Qt::UserRole + 1 };
    explicit ActivityModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QString preferenceId() const override;
    QString settingsTitle() const override;
    QList<SettingField> settingsFields() const override;
    QJsonObject savePreferences() const override;
    void restorePreferences(const QJsonObject &values) override;
    void clear();
signals:
    void latest(const QString &message, bool error);
private:
    void collect();
    QList<ActivityEvent> events_;
    quint64 sequence_ = 0;
    int capacity_ = 500;
    QTimer timer_;
};
}

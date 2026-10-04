#pragma once
#include "core/event.h"
#include "core/settings.h"
#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QPointF>
#include <QTimer>
namespace swak {
struct VisualEvent { Event event; QPointF world; double opacity = 1; double gray = 0; bool expired = false; };
class TrackMotion final : public QObject, public SettingsParticipant {
    Q_OBJECT
public:
    explicit TrackMotion(QObject *parent = nullptr);
    QString preferenceId() const override { return QStringLiteral("org.swak.track-motion"); }
    QString settingsTitle() const override { return tr("CoT animation"); }
    QList<SettingField> settingsFields() const override;
    QJsonObject savePreferences() const override;
    void restorePreferences(const QJsonObject &values) override;
    void update(const Event &event, qint64 now = -1);
    void expire(const QString &uid, qint64 now = -1);
    void clear();
    QList<VisualEvent> snapshot(qint64 now = -1) const;
    qint64 time() const { return clock_.elapsed(); }
signals:
    void changed();
private:
    struct Motion { Event event; QPointF from, to; qint64 start = 0, stale = -1; bool teleport = false; double initialOpacity = 1; };
    VisualEvent visual(const Motion &motion, qint64 now) const;
    void advance();
    QHash<QString, Motion> records_;
    QElapsedTimer clock_;
    QTimer timer_;
    int duration_ = 600;
    double threshold_ = 100;
    int staleSeconds_ = 10;
    int fade_ = 600;
};
}

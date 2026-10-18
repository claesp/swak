#pragma once
#include "core/event.h"
#include "core/settings.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QTimer>

#include <optional>
namespace swak {
struct TrackKinematics
{
    std::optional<double> speed = {};
    std::optional<double> heading = {};
    bool estimatedHeading = false;
    bool calculatedSpeed = false;
};
struct HeadingConeStyle
{
    bool enabled = true;
    double angle = 60;
    double length = 60;
    double opacity = .15;
    // Zero width draws no outline.
    double outlineWidth = 0;
    double outlineOpacity = .6;
    bool operator==(const HeadingConeStyle &) const = default;
};
struct VisualEvent
{
    Event event;
    QPointF world;
    double opacity = 1;
    double gray = 0;
    bool expired = false;
    TrackKinematics kinematics = {};
};
class TrackMotion final : public QObject, public SettingsParticipant
{
    Q_OBJECT
public:
    explicit TrackMotion(QObject *parent = nullptr);
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.track-motion");
    }
    QString settingsTitle() const override
    {
        return tr("CoT animation");
    }
    QList<SettingField> settingsFields() const override;
    QJsonObject savePreferences() const override;
    QJsonObject animationPreferences() const;
    void restorePreferences(const QJsonObject &values) override;
    void update(const Event &event, qint64 now = -1);
    void expire(const QString &uid, qint64 now = -1);
    void clear();
    void remove(const QString &uid);
    bool paused() const
    {
        return paused_;
    }
    void setPaused(bool paused);
    QList<VisualEvent> snapshot(qint64 now = -1) const;
    TrackKinematics kinematics(const QString &uid) const;
    const HeadingConeStyle &headingConeStyle() const
    {
        return cone_;
    }
    bool headingConeEnabled(const QString &uid) const
    {
        return coneTargets_.contains(uid);
    }
    bool canEnableHeadingCone(const QString &uid) const
    {
        return coneTargets_.contains(uid) || coneTargets_.size() < 4096;
    }
    void setHeadingConeEnabled(const QString &uid, bool enabled);
    // The FOV settings own the cone style. The style is not part of the animation settings.
    void setHeadingConeStyle(const HeadingConeStyle &style);
    qint64 time() const
    {
        return clock_.elapsed();
    }
signals:
    void changed();
    void pauseChanged(bool paused);
    void coneTargetsChanged();
    void configurationChanged();

private:
    struct Motion
    {
        Event event;
        QPointF from, to;
        qint64 start = 0, stale = -1;
        bool teleport = false;
        double initialOpacity = 1;
        QPointF velocity = {};
        qint64 horizon = 0;
        TrackKinematics kinematics = {};
    };
    VisualEvent visual(const Motion &motion, qint64 now) const;
    void advance();
    QHash<QString, Motion> records_;
    QList<VisualEvent> frozen_;
    bool paused_ = false;
    QElapsedTimer clock_;
    QTimer timer_;
    int duration_ = 600;
    int prediction_ = 3000;
    double threshold_ = 100;
    int staleSeconds_ = 10;
    int fade_ = 600;
    HeadingConeStyle cone_;
    QSet<QString> coneTargets_;
};
} // namespace swak

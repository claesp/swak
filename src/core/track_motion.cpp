#include "core/track_motion.h"
#include "core/projection.h"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace swak {
TrackMotion::TrackMotion(QObject *parent) : QObject(parent)
{
    clock_.start(); timer_.setInterval(33); connect(&timer_, &QTimer::timeout, this, &TrackMotion::advance);
}
QList<SettingField> TrackMotion::settingsFields() const
{
    return {numberSetting(QStringLiteral("predictionMs"), tr("Prediction limit (milliseconds)"), 0, 5000), numberSetting(QStringLiteral("duration"), tr("Movement duration (milliseconds)"), 0, 5000),
        numberSetting(QStringLiteral("teleportKm"), tr("Teleport distance (kilometers)"), 0.01, 20000, 2),
        numberSetting(QStringLiteral("fade"), tr("Fade duration (milliseconds)"), 100, 5000),
        numberSetting(QStringLiteral("staleSeconds"), tr("Gray retention (seconds)"), 0, 300)};
}
QJsonObject TrackMotion::savePreferences() const { return {{QStringLiteral("predictionMs"), prediction_}, {QStringLiteral("duration"), duration_}, {QStringLiteral("teleportKm"), threshold_}, {QStringLiteral("fade"), fade_}, {QStringLiteral("staleSeconds"), staleSeconds_}}; }
void TrackMotion::restorePreferences(const QJsonObject &v)
{
    prediction_ = preferenceInteger(v, QStringLiteral("predictionMs"), 3000, 0, 5000);
    duration_ = preferenceInteger(v, QStringLiteral("duration"), 600, 0, 5000);
    threshold_ = preferenceNumber(v, QStringLiteral("teleportKm"), 100, 0.01, 20000);
    fade_ = preferenceInteger(v, QStringLiteral("fade"), 600, 100, 5000);
    staleSeconds_ = preferenceInteger(v, QStringLiteral("staleSeconds"), 10, 0, 300);
    for (auto &record : records_) { record.from = record.to; record.start = time() - std::max(duration_, fade_); record.initialOpacity = 1; record.velocity = {}; record.horizon = 0; }
    if (!records_.isEmpty()) if (!paused_) timer_.start();
    emit changed();
}
VisualEvent TrackMotion::visual(const Motion &m, qint64 now) const
{
    VisualEvent v{m.event, m.to};
    if (m.teleport) {
        const double progress = std::clamp(static_cast<double>(now - m.start) / fade_, 0.0, 1.0);
        if (progress < 0.5) { v.world = m.from; v.opacity = m.initialOpacity * (1 - progress * 2); }
        else v.opacity = (progress - 0.5) * 2;
    } else if (duration_ > 0) {
        const double progress = std::clamp(static_cast<double>(now - m.start) / duration_, 0.0, 1.0);
        auto delta = m.to - m.from; delta.setX(delta.x() - std::round(delta.x()));
        v.world = m.from + delta * progress; v.world.setX(v.world.x() - std::floor(v.world.x()));
        v.opacity = m.initialOpacity + (1 - m.initialOpacity) * progress;
    }
    if (!m.teleport && duration_ > 0 && m.horizon > 0 && m.stale < 0) {
        const double age = static_cast<double>(std::max<qint64>(0, now - m.start));
        const double horizon = static_cast<double>(m.horizon);
        const double coast = horizon * 0.75;
        const double braking = std::clamp(age - coast, 0.0, horizon - coast);
        const double distanceTime = std::min(age, coast) + braking - braking * braking / (2 * (horizon - coast));
        auto correction = m.from - m.to; correction.setX(correction.x() - std::round(correction.x()));
        const double p = std::clamp(age / duration_, 0.0, 1.0);
        v.world = m.to + m.velocity * distanceTime + correction * (1 - p * p * (3 - 2 * p));
        v.world.setX(v.world.x() - std::floor(v.world.x()));
        v.world.setY(std::clamp(v.world.y(), 0.0, 1.0));
    }
    if (m.stale >= 0) {
        v.expired = true;
        const auto age = std::max<qint64>(0, now - m.stale);
        v.gray = std::clamp(static_cast<double>(age) / fade_, 0.0, 1.0);
        v.opacity *= 1.0 - 0.45 * v.gray;
        if (age > fade_ + staleSeconds_ * 1000) v.opacity *= std::clamp(1.0 - static_cast<double>(age - fade_ - staleSeconds_ * 1000) / fade_, 0.0, 1.0);
    }
    return v;
}
void TrackMotion::update(const Event &event, qint64 now)
{
    if (now < 0) now = time();
    const auto target = project(event.latitude, event.longitude);
    auto it = records_.find(event.uid);
    if (it == records_.end()) {
        if (records_.size() >= MaxEvents * 2) {
            auto oldest = records_.end();
            for (auto candidate = records_.begin(); candidate != records_.end(); ++candidate) if (candidate->stale >= 0 && (oldest == records_.end() || candidate->stale < oldest->stale)) oldest = candidate;
            if (oldest == records_.end()) return;
            records_.erase(oldest);
        }
        records_.insert(event.uid, {event, target, target, now - std::max(duration_, fade_)}); return;
    }
    if (paused_) { *it = {event, target, target, now - std::max(duration_, fade_)}; return; }
    auto &record = *it;
    if (record.to == target && record.stale < 0 && record.velocity.isNull()) { record.event = event; return; }
    if (record.teleport && record.stale < 0 && now - record.start < fade_) {
        record.event = event; record.to = target; timer_.start(); return;
    }
    const auto current = visual(record, now);
    const auto position = unproject(current.world);
    const auto radians = std::numbers::pi / 180.0;
    const auto latDelta = (event.latitude - position.y()) * radians;
    const auto lonDelta = (event.longitude - position.x()) * radians;
    const double a = std::pow(std::sin(latDelta / 2), 2) + std::cos(position.y() * radians) * std::cos(event.latitude * radians) * std::pow(std::sin(lonDelta / 2), 2);
    const double km = 12742 * std::asin(std::sqrt(std::clamp(a, 0.0, 1.0)));
    auto delta = target - record.to; delta.setX(delta.x() - std::round(delta.x()));
    const auto interval = record.event.time.msecsTo(event.time);
    const bool predict = prediction_ > 0 && duration_ > 0 && interval >= 50 && interval <= 10000 && km <= threshold_
        && record.stale < 0 && !record.teleport && !delta.isNull()
        && std::hypot(delta.x(), delta.y()) * 40075000 / static_cast<double>(std::max<qint64>(1, interval)) <= 1.0;
    record = {event, current.world, target, now, -1, km > threshold_, current.opacity};
    if (predict) { record.velocity = delta / static_cast<double>(interval); record.horizon = std::min<qint64>(prediction_, interval * 2); }
    timer_.start();
}
void TrackMotion::expire(const QString &uid, qint64 now)
{
    auto it = records_.find(uid); if (it == records_.end() || it->stale >= 0) return;
    if (now < 0) now = time();
    const auto current = visual(*it, now);
    it->from = it->to = current.world; it->initialOpacity = 1; it->start = now - std::max(duration_, fade_); it->stale = now; it->velocity = {}; it->horizon = 0;
    if (!paused_) timer_.start();
}
QList<VisualEvent> TrackMotion::snapshot(qint64 now) const
{
    if (paused_) return frozen_;
    if (now < 0) now = time();
    QList<VisualEvent> result; result.reserve(records_.size());
    for (const auto &record : records_) { auto v = visual(record, now); if (v.opacity > 0) result.append(std::move(v)); }
    return result;
}
void TrackMotion::advance()
{
    bool active = false; const auto now = time();
    for (auto it = records_.begin(); it != records_.end();) {
        if (it->stale >= 0 && now - it->stale >= fade_ * 2 + staleSeconds_ * 1000) { it = records_.erase(it); continue; }
        active = active || it->stale >= 0 || now - it->start < (it->teleport ? fade_ : std::max<qint64>(duration_, it->horizon)); ++it;
    }
    emit changed(); if (!active) timer_.stop();
}
void TrackMotion::setPaused(bool paused)
{
    if (paused == paused_) return;
    if (paused) { frozen_ = snapshot(); paused_ = true; timer_.stop(); emit pauseChanged(true); emit changed(); return; }
    paused_ = false; frozen_.clear(); emit pauseChanged(false);
    for (auto &record : records_) if (record.stale < 0) {
        record.from = record.to; record.start = time() - std::max(duration_, fade_);
        record.teleport = false; record.initialOpacity = 1; record.velocity = {}; record.horizon = 0;
    }
    advance();
    if (std::any_of(records_.cbegin(), records_.cend(), [](const Motion &record) { return record.stale >= 0; })) timer_.start();
}
void TrackMotion::clear() { frozen_.clear(); records_.clear(); timer_.stop(); emit changed(); }
}

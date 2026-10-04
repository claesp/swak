#include "core/prediction.h"

#include "core/measure.h"
#include "core/projection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace swak {
namespace {
constexpr double Radius = 6371000;
constexpr double Radians = std::numbers::pi / 180.0;
constexpr double MaxSpeed = 1000;
} // namespace
std::optional<GroundMotion> groundMotion(QPointF first, QPointF last, qint64 intervalMs)
{
    const auto valid = [](QPointF p) {
        return std::isfinite(p.x()) && std::isfinite(p.y()) && p.x() >= 0 && p.x() <= 1 &&
               p.y() >= 0 && p.y() <= 1;
    };
    if (!valid(first) || !valid(last) || intervalMs < 50 || intervalMs > 600000) {
        return {};
    }
    const double distance = groundDistance(first, last);
    const double speed = distance * 1000 / static_cast<double>(intervalMs);
    if (!std::isfinite(speed) || speed > MaxSpeed) {
        return {};
    }
    GroundMotion result{speed};
    if (distance >= .01) {
        const auto a = unproject(first), b = unproject(last);
        const double latitude = a.y() * Radians, nextLatitude = b.y() * Radians;
        const double longitude = std::remainder((b.x() - a.x()) * Radians, 2 * std::numbers::pi);
        const double heading =
            std::atan2(std::sin(longitude) * std::cos(nextLatitude),
                       std::cos(latitude) * std::sin(nextLatitude) -
                           std::sin(latitude) * std::cos(nextLatitude) * std::cos(longitude)) /
            Radians;
        result.heading = std::fmod(heading + 360, 360);
    }
    return result;
}
void PredictionHistory::append(const Event &event, double teleportKm, int historySeconds)
{
    if (!event.time.isValid() || !std::isfinite(event.latitude) ||
        !std::isfinite(event.longitude) || std::abs(event.latitude) > 90 ||
        std::abs(event.longitude) > 180 || !std::isfinite(teleportKm) || teleportKm <= 0) {
        return;
    }
    const auto point = project(event.latitude, event.longitude);
    if (!samples_.isEmpty()) {
        const auto elapsed = samples_.last().time.msecsTo(event.time);
        if (elapsed == 0 && point != samples_.last().world) {
            samples_.clear();
            samples_.append({event.time, point});
            return;
        }
        if (elapsed < 50) {
            return;
        }
        const auto distance = groundDistance(samples_.last().world, point);
        if (elapsed > std::clamp(historySeconds, 10, 600) * 1000 || distance > teleportKm * 1000 ||
            distance / (static_cast<double>(elapsed) / 1000) > MaxSpeed) {
            samples_.clear();
        }
    }
    samples_.append({event.time, point});
    while (samples_.size() > 50 ||
           samples_.first().time.secsTo(event.time) > std::clamp(historySeconds, 10, 600)) {
        samples_.removeFirst();
    }
}
std::optional<QPointF> PredictionHistory::forecast(int seconds) const
{
    if (samples_.size() < 2 || seconds < 0 || seconds > 1800) {
        return {};
    }
    const auto segment = [](const Sample &first, const Sample &last) {
        const auto interval = first.time.msecsTo(last.time);
        const auto motion =
            groundMotion(first.world, last.world, interval).value_or(GroundMotion{});
        return std::array<double, 3>{motion.speed,
                                     motion.heading.value_or(0) * Radians,
                                     static_cast<double>(interval) / 1000};
    };
    const auto latest = segment(samples_.at(samples_.size() - 2), samples_.last());
    double speed = latest[0], bearing = latest[1];
    double acceleration = 0, turn = 0;
    if (samples_.size() >= 3) {
        const auto previous =
            segment(samples_.at(samples_.size() - 3), samples_.at(samples_.size() - 2));
        const double interval = (previous[2] + latest[2]) / 2;
        acceleration = std::clamp((speed - previous[0]) / interval, -5.0, 5.0);
        if (speed >= .5 && previous[0] >= .5) {
            turn =
                std::clamp(std::remainder(bearing - previous[1], 2 * std::numbers::pi) / interval,
                           -5 * Radians,
                           5 * Radians);
        }
    }
    auto position = unproject(samples_.last().world);
    // Decay recent trends instead of extending acceleration or turns for half an hour.
    for (int elapsed = 0; elapsed < seconds; elapsed += 5) {
        const double duration = std::min(5, seconds - elapsed);
        const double trend =
            30 * (std::exp(-elapsed / 30.0) - std::exp(-(elapsed + duration) / 30.0));
        const double nextSpeed = std::clamp(speed + acceleration * trend, 0.0, MaxSpeed);
        const double change = turn * trend;
        const double direction = bearing + change / 2;
        const double distance = (speed + nextSpeed) / 2 * duration / Radius;
        const double latitude = position.y() * Radians;
        const double targetLatitude =
            std::asin(std::clamp(std::sin(latitude) * std::cos(distance) +
                                     std::cos(latitude) * std::sin(distance) * std::cos(direction),
                                 -1.0,
                                 1.0));
        const double longitude =
            position.x() * Radians +
            std::atan2(std::sin(direction) * std::sin(distance) * std::cos(latitude),
                       std::cos(distance) - std::sin(latitude) * std::sin(targetLatitude));
        position = QPointF(std::remainder(longitude / Radians, 360.0), targetLatitude / Radians);
        speed = nextSpeed;
        bearing += change;
    }
    return project(position.y(), position.x());
}
} // namespace swak

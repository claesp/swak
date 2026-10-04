#include "core/prediction.h"

#include "core/measure.h"
#include "core/projection.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace swak {
namespace {
constexpr double Radius = 6371000;
constexpr double Radians = std::numbers::pi / 180.0;
constexpr double MaxSpeed = 1000;
} // namespace
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
    // Fit velocity to recent positions. Unwrap longitude before the fit.
    double meanTime = 0;
    QPointF meanPoint;
    QPointF previous = samples_.first().world;
    QList<QPointF> points;
    points.reserve(samples_.size());
    for (const auto &sample : samples_) {
        auto point = sample.world;
        point.setX(point.x() - std::round(point.x() - previous.x()));
        points.append(point);
        previous = point;
        meanPoint += point;
        meanTime += static_cast<double>(samples_.last().time.msecsTo(sample.time)) / 1000;
    }
    const auto count = static_cast<double>(samples_.size());
    meanPoint /= count;
    meanTime /= count;
    double variance = 0;
    QPointF covariance;
    for (qsizetype i = 0; i < samples_.size(); ++i) {
        const auto time =
            static_cast<double>(samples_.last().time.msecsTo(samples_.at(i).time)) / 1000 -
            meanTime;
        covariance += (points.at(i) - meanPoint) * time;
        variance += time * time;
    }
    if (variance <= 0) {
        return {};
    }
    const auto position = unproject(samples_.last().world);
    const double latitude = position.y() * Radians;
    const auto velocity =
        covariance / variance * (2 * std::numbers::pi * Radius * std::cos(latitude));
    const auto speed = std::hypot(velocity.x(), velocity.y());
    if (!std::isfinite(speed) || speed > MaxSpeed) {
        return {};
    }
    const auto bearing = std::atan2(velocity.x(), -velocity.y());
    const auto distance = speed * seconds / Radius;
    const auto targetLatitude =
        std::asin(std::clamp(std::sin(latitude) * std::cos(distance) +
                                 std::cos(latitude) * std::sin(distance) * std::cos(bearing),
                             -1.0,
                             1.0));
    const auto targetLongitude =
        position.x() * Radians +
        std::atan2(std::sin(bearing) * std::sin(distance) * std::cos(latitude),
                   std::cos(distance) - std::sin(latitude) * std::sin(targetLatitude));
    return project(targetLatitude / Radians, std::remainder(targetLongitude / Radians, 360.0));
}
} // namespace swak

#include "tak_load_meter.h"

#include <algorithm>
#include <cmath>

namespace {
double percentile(std::vector<qint64> values, double fraction)
{
    if (values.empty()) {
        return -1;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(
        std::clamp(std::ceil(fraction * static_cast<double>(values.size())) - 1.0,
                   0.0,
                   static_cast<double>(values.size() - 1)));
    return static_cast<double>(values[index]);
}
} // namespace

LoadMeter::LoadMeter(qint64 positions)
    : sentAt_(static_cast<size_t>(std::clamp<qint64>(positions, 0, MaxPositions)), NotSent)
{
}
void LoadMeter::sent(qint64 sequence, qint64 milliseconds)
{
    if (sequence < 0 || sequence >= positions() || milliseconds < 0 ||
        sentAt_[static_cast<size_t>(sequence)] != NotSent) {
        return;
    }
    sentAt_[static_cast<size_t>(sequence)] = milliseconds;
    ++sent_;
}
void LoadMeter::received(qint64 sequence, qint64 milliseconds)
{
    if (sequence < 0 || sequence >= positions()) {
        return;
    }
    auto &sentTime = sentAt_[static_cast<size_t>(sequence)];
    if (sentTime == NotSent) {
        return;
    }
    if (sentTime == Received) {
        ++duplicates_;
        return;
    }
    latencies_.push_back(std::max<qint64>(0, milliseconds - sentTime));
    sentTime = Received;
}
void LoadMeter::connection(bool connected)
{
    if (connected_ && !connected) {
        ++disconnects_;
    }
    connected_ = connected;
}
QJsonObject LoadMeter::result(double minimumRatio) const
{
    const auto delivered = deliveredCount();
    const double ratio =
        sent_ > 0 ? static_cast<double>(delivered) / static_cast<double>(sent_) : 0;
    const bool passed = sent_ > 0 && ratio >= std::clamp(minimumRatio, 0.0, 1.0) &&
                        duplicates_ == 0 && disconnects_ == 0;
    return QJsonObject{{QStringLiteral("passed"), passed},
                       {QStringLiteral("sent"), static_cast<double>(sent_)},
                       {QStringLiteral("delivered"), static_cast<double>(delivered)},
                       {QStringLiteral("ratio"), ratio},
                       {QStringLiteral("duplicates"), static_cast<double>(duplicates_)},
                       {QStringLiteral("disconnects"), disconnects_},
                       {QStringLiteral("latencyMsP50"), percentile(latencies_, 0.50)},
                       {QStringLiteral("latencyMsP95"), percentile(latencies_, 0.95)},
                       {QStringLiteral("latencyMsMax"), percentile(latencies_, 1.0)}};
}

#pragma once
#include <QJsonObject>

#include <vector>

// Counts the positions of a load run. Each position has a sequence number. The meter records
// its send time, the first receipt as a latency, and every later receipt as a repeat. It also
// counts the moves from connected to disconnected.
class LoadMeter
{
public:
    // The meter keeps one time for each position, so it limits their number.
    static constexpr qint64 MaxPositions = 2'000'000;
    explicit LoadMeter(qint64 positions);
    qint64 positions() const
    {
        return static_cast<qint64>(sentAt_.size());
    }
    // Records a sent position. Call it only after a successful send.
    void sent(qint64 sequence, qint64 milliseconds);
    // Records a received position. Unknown and unsent sequence numbers are ignored.
    void received(qint64 sequence, qint64 milliseconds);
    // Records the connection state after each send.
    void connection(bool connected);
    qint64 sentCount() const
    {
        return sent_;
    }
    qint64 deliveredCount() const
    {
        return static_cast<qint64>(latencies_.size());
    }
    // The counters, the delivery ratio, the latency percentiles, and passed. The run passes
    // when the ratio reaches the minimum without a repeat or a disconnect.
    QJsonObject result(double minimumRatio) const;

private:
    // The send time of each position in milliseconds, or one of these states.
    static constexpr qint64 NotSent = -2;
    static constexpr qint64 Received = -1;
    std::vector<qint64> sentAt_;
    std::vector<qint64> latencies_;
    qint64 sent_ = 0;
    qint64 duplicates_ = 0;
    int disconnects_ = 0;
    bool connected_ = true;
};

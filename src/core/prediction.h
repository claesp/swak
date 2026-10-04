#pragma once
#include "core/event.h"

#include <QList>
#include <QPointF>

#include <optional>

namespace swak {
class PredictionHistory final
{
public:
    void append(const Event &event, double teleportKm, int historySeconds = 300);
    std::optional<QPointF> forecast(int seconds) const;
    qsizetype size() const
    {
        return samples_.size();
    }

private:
    struct Sample
    {
        QDateTime time;
        QPointF world;
    };
    QList<Sample> samples_;
};
} // namespace swak

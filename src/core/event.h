#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <optional>

namespace swak {
inline constexpr qsizetype MaxMessageBytes = 16 * 1024;
inline constexpr int MaxEvents = 4096;

struct Event
{
    QString uid;
    QString type;
    QString how;
    QString callsign;
    QString team;
    QString role;
    QDateTime time;
    QDateTime start;
    QDateTime stale;
    double latitude = 0;
    double longitude = 0;
    double altitude = 0;
    double circularError = 9999999;
    double linearError = 9999999;
};

struct DecodeResult
{
    std::optional<Event> event;
    QString error;
};

QString eventError(const Event &event);
} // namespace swak

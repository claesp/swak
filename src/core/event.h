#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <optional>

namespace swak {
// ATAK routes and shapes carry many link points, so one message can hold 64 KiB.
inline constexpr qsizetype MaxMessageBytes = 64 * 1024;
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
    std::optional<double> course = {};
    std::optional<double> speed = {};
    // All detail children as canonical, escaped XML, without the detail element itself.
    // It holds at most MaxDetailCharacters. Read it through core/cot_detail.h.
    QString detail;
    // True when the received detail exceeded the retained size. Typed fields remain valid.
    bool detailTruncated = false;
    // The server entry that received the event. Empty for local events. A TAK delete event
    // removes only events with the same origin.
    QString origin;
};

struct DecodeResult
{
    std::optional<Event> event;
    QString error;
};

QString eventError(const Event &event);
} // namespace swak

#include "cot_simulation.h"
#include <QJsonArray>
#include <QRandomGenerator>
#include <cmath>
#include <algorithm>
#include <numbers>
namespace swak::simulation {
namespace {
constexpr double Pi = std::numbers::pi;
constexpr double Earth = 6371000;
bool number(const QJsonObject &object, const char *key, double low, double high, double &result)
{
    const auto value = object.value(QLatin1String(key)); result = value.toDouble();
    return value.isDouble() && std::isfinite(result) && result >= low && result <= high;
}
}
QJsonObject defaults()
{
    return {{"latitude", 59.33}, {"longitude", 18.0}, {"frequency", 1}, {"lifetime", 15}, {"radius", 2000}, {"separation", 3000}, {"seed", 1},
        {"teams", QJsonArray{QJsonObject{{"name", "Alpha"}, {"team", "Blue"}, {"affiliation", "f"}}, QJsonObject{{"name", "Bravo"}, {"team", "Red"}, {"affiliation", "h"}}}},
        {"types", QJsonArray{
            QJsonObject{{"name", "Infantry"}, {"type", "G-U-C-I"}, {"count", 6}, {"speed", 1.4}, {"altitude", 20}},
            QJsonObject{{"name", "Vehicle"}, {"type", "G-E-V-A"}, {"count", 3}, {"speed", 12}, {"altitude", 20}},
            QJsonObject{{"name", "Helicopter"}, {"type", "A-M-H"}, {"count", 1}, {"speed", 45}, {"altitude", 300}},
            QJsonObject{{"name", "Aircraft"}, {"type", "A-M-F"}, {"count", 1}, {"speed", 110}, {"altitude", 2500}}}}};
}
Scenario load(const QJsonObject &input, const QString &session)
{
    auto values = defaults();
    for (auto it = input.begin(); it != input.end(); ++it) {
        if (!values.contains(it.key())) { Scenario invalid; invalid.error = QStringLiteral("Unknown scenario field."); return invalid; }
        values.insert(it.key(), it.value());
    }
    Scenario result; double radius = 0, separation = 0, seed = 0, lifetime = 0;
    const auto fail = [&result](const char *message) { result.error = QString::fromLatin1(message); return result; };
    if (!number(values, "latitude", -80, 80, result.latitude) || !number(values, "longitude", -180, 180, result.longitude) ||
        !number(values, "frequency", 0.1, 20, result.frequency) || !number(values, "lifetime", 1, 3600, lifetime) || lifetime != std::floor(lifetime) ||
        lifetime <= 1 / result.frequency || !number(values, "radius", 100, 50000, radius) ||
        !number(values, "separation", 0, 100000, separation) || !number(values, "seed", 0, 4294967295.0, seed) || seed != std::floor(seed))
        return fail("Use valid coordinates, positive rates, and a lifetime longer than the update interval.");
    result.lifetime = static_cast<int>(lifetime);
    if (!values.value("teams").isArray() || !values.value("types").isArray()) return fail("Teams and types must be arrays.");
    const auto teams = values.value("teams").toArray(); const auto types = values.value("types").toArray();
    if (teams.size() != 2 || types.isEmpty() || types.size() > 16) return fail("Use two teams and one to sixteen unit types.");
    QRandomGenerator random(static_cast<quint32>(seed));
    for (int t = 0; t < teams.size(); ++t) {
        if (!teams.at(t).isObject()) return fail("Each team must be an object.");
        const auto team = teams.at(t).toObject(); const auto name = team.value("name").toString();
        const auto affiliation = team.value("affiliation").toString();
        if (name.isEmpty() || name.size() > 32 || affiliation.size() != 1 || !QStringLiteral("puafnshjko").contains(affiliation)) return fail("A team name or affiliation is invalid.");
        int index = 0;
        for (const auto &entry : types) {
            if (!entry.isObject()) return fail("Each unit type must be an object.");
            const auto type = entry.toObject(); double count = 0, speed = 0, altitude = 0;
            if (!number(type, "count", 0, 512, count) || count != std::floor(count) || !number(type, "speed", 0, 400, speed) ||
                !number(type, "altitude", -500, 20000, altitude)) return fail("A unit count, speed, or altitude is outside its supported range.");
            const auto label = type.value("name").toString(); const auto code = type.value("type").toString();
            if (label.isEmpty() || label.size() > 32 || (code.section('-', 0, 0) != "G" && code.section('-', 0, 0) != "A")) return fail("Use a named ground or air type suffix.");
            Event prototype; prototype.uid = session + QStringLiteral("-%1-%2").arg(t).arg(index); prototype.callsign = name + '-' + label;
            prototype.type = QStringLiteral("a-") + affiliation + '-' + code; prototype.team = team.value("team").toString();
            prototype.role = QStringLiteral("Team Member"); prototype.how = QStringLiteral("m-s"); prototype.altitude = altitude;
            prototype.time = prototype.start = QDateTime::currentDateTimeUtc(); prototype.stale = prototype.time.addSecs(result.lifetime);
            prototype.circularError = code.startsWith('A') ? 15 : 5; prototype.linearError = code.startsWith('A') ? 25 : 10;
            if (!eventError(prototype).isEmpty()) return fail("A generated identity or type contains invalid text.");
            for (int i = 0; i < static_cast<int>(count); ++i) {
                if (result.units.size() >= 1024) return fail("Use at most 1024 units in total.");
                Unit unit; unit.event = prototype; unit.event.uid = session + QStringLiteral("-%1-%2").arg(t).arg(index++);
                unit.event.callsign += QStringLiteral("-%1").arg(i + 1, 3, 10, QLatin1Char('0'));
                if (index == 1) unit.event.role = QStringLiteral("Team Lead");
                unit.speed = speed; unit.radius = radius * (code.startsWith('A') ? 2 : 1);
                unit.phase = random.generateDouble() * 2 * Pi; unit.direction = t ? -1 : 1;
                unit.east = (t ? 0.5 : -0.5) * separation;
                unit.north = (random.generateDouble() - 0.5) * radius * 0.15;
                if (!eventError(unit.event).isEmpty()) return fail("A generated unit identity is too long or invalid.");
                result.units.append(unit);
            }
        }
        if (!index) return fail("Each team must contain at least one unit.");
    }
    if (static_cast<double>(result.units.size()) * result.frequency > 2000) return fail("Use at most 2000 messages per second.");
    return result;
}
Event sample(const Scenario &scenario, const Unit &unit, double elapsedSeconds, const QDateTime &now)
{
    if (!std::isfinite(elapsedSeconds) || !now.isValid() || !std::isfinite(unit.radius) || unit.radius <= 0) return {};
    Event event = unit.event;
    const auto angle = unit.phase + unit.direction * std::fmod(std::max(0.0, elapsedSeconds) * unit.speed / unit.radius, 2 * Pi);
    const auto east = unit.east + unit.radius * std::cos(angle);
    const auto north = unit.north + unit.radius * std::sin(angle);
    const auto distance = std::hypot(east, north) / Earth;
    const auto bearing = std::atan2(east, north);
    const auto lat = scenario.latitude * Pi / 180, lon = scenario.longitude * Pi / 180;
    const auto nextLat = std::asin(std::sin(lat) * std::cos(distance) + std::cos(lat) * std::sin(distance) * std::cos(bearing));
    const auto nextLon = lon + std::atan2(std::sin(bearing) * std::sin(distance) * std::cos(lat), std::cos(distance) - std::sin(lat) * std::sin(nextLat));
    event.latitude = nextLat * 180 / Pi; event.longitude = std::remainder(nextLon * 180 / Pi, 360.0);
    event.time = event.start = now; event.stale = now.addSecs(scenario.lifetime);
    return event;
}
}

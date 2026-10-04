#pragma once
#include "core/event.h"

#include <QJsonObject>
#include <QList>
namespace swak::simulation {
struct Unit
{
    Event event;
    double speed = 0;
    double radius = 0;
    double phase = 0;
    double east = 0;
    double north = 0;
    int direction = 1;
};
struct Scenario
{
    QList<Unit> units;
    double latitude = 59.33;
    double longitude = 18.0;
    double frequency = 1;
    int lifetime = 15;
    QString error;
};
QJsonObject defaults();
Scenario load(const QJsonObject &values, const QString &session);
Event sample(const Scenario &scenario,
             const Unit &unit,
             double elapsedSeconds,
             const QDateTime &now);
} // namespace swak::simulation

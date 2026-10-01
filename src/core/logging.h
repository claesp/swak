#pragma once
#include <QJsonObject>
#include <QString>
#include <QtLogging>

namespace swak {
enum class Severity { Debug, Info, Warning, Error, Fatal };
enum class Urgency { Routine, Attention, Immediate };

QtMessageHandler installLogHandler();
void configureLogging(const QJsonObject &values);
void configureLoggingFromFile(const QString &path);
QJsonObject loggingConfiguration();
void logMessage(Severity severity, Urgency urgency, const QString &category, const QString &message) noexcept;
}

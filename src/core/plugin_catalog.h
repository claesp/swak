#pragma once
#include <QJsonObject>
#include <QList>
#include <QString>
class QObject;
namespace swak {
QString validatePluginManifest(const QJsonObject &manifest, QObject *instance);
QList<QObject *> builtInPlugins();
QJsonObject pluginManifest(QObject *instance);
} // namespace swak

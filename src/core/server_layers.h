#pragma once
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace swak {
// One TAK Server map layer as a map provider entry for the GDAL raster provider.
struct ServerMapLayer
{
    QString id;
    QString name;
    QJsonObject source;
};
inline constexpr int MaxServerMapLayers = 256;

// Reads a /Marti/api/maplayers/all reply. XYZ and WMS layers become map provider entries
// with HTTPS endpoints. Other layers go to skipped with a reason. Returns false for other
// content.
bool readServerMapLayers(const QByteArray &json,
                         const QString &serverId,
                         const QString &serverName,
                         QList<ServerMapLayer> *layers,
                         QStringList *skipped);
} // namespace swak

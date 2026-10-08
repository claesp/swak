#include "core/server_layers.h"

#include "core/maps.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace swak {
namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("ServerLayers", text);
}
QString plain(const QString &text, qsizetype limit)
{
    QString result;
    for (const auto ch : text.left(limit)) {
        if (ch.category() != QChar::Other_Control && ch.category() != QChar::Other_Format &&
            !ch.isSurrogate()) {
            result += ch;
        }
    }
    return result.trimmed();
}
// Map provider identifiers allow letters, digits, periods, and hyphens.
QString idPart(const QString &text)
{
    QString result;
    for (const auto ch : text.left(40)) {
        result += (ch.isLetterOrNumber() && ch.unicode() < 128) || ch == u'.' || ch == u'-'
                      ? ch
                      : QChar(u'-');
    }
    return result;
}
int zoom(const QJsonValue &value, int fallback)
{
    return value.isDouble() ? std::clamp(value.toInt(fallback), 0, 19) : fallback;
}
} // namespace

bool readServerMapLayers(const QByteArray &json,
                         const QString &serverId,
                         const QString &serverName,
                         QList<ServerMapLayer> *layers,
                         QStringList *skipped)
{
    const auto document = QJsonDocument::fromJson(json);
    const auto list = document.isArray()
                          ? document.array()
                          : document.object().value(QStringLiteral("data")).toArray();
    if ((!document.isArray() && !document.object().value(QStringLiteral("data")).isArray()) ||
        list.size() > MaxServerMapLayers) {
        return false;
    }
    QList<ServerMapLayer> result;
    QStringList reasons;
    QSet<QString> ids;
    const auto attribution = tr("TAK Server %1").arg(serverName).left(300);
    for (const auto &value : list) {
        const auto layer = value.toObject();
        const auto name = plain(layer.value(QStringLiteral("name")).toString(), 100);
        const auto label = name.isEmpty() ? tr("Unnamed layer") : name;
        const auto type = layer.value(QStringLiteral("type")).toString().toLower();
        auto url = layer.value(QStringLiteral("url")).toString().trimmed();
        // ATAK and MOBAC templates use {$z}, {$x}, {$y}, and {$serverpart}.
        url.replace(QStringLiteral("{$z}"), QStringLiteral("{z}"))
            .replace(QStringLiteral("{$x}"), QStringLiteral("{x}"))
            .replace(QStringLiteral("{$y}"), QStringLiteral("{y}"));
        const auto parts = layer.value(QStringLiteral("serverParts"))
                               .toString()
                               .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        url.replace(QStringLiteral("{$serverpart}"), parts.isEmpty() ? QString{} : parts.first());
        QString service;
        if (type == u"wms") {
            service = QStringLiteral("wms");
        } else if (type == u"wmts") {
            reasons.append(
                tr("%1: WMTS layers need a matrix set. Add them in Map Providers.").arg(label));
            continue;
        } else if (url.contains(QStringLiteral("{z}")) && url.contains(QStringLiteral("{x}")) &&
                   url.contains(QStringLiteral("{y}"))) {
            service = QStringLiteral("xyz");
        } else {
            reasons.append(tr("%1: SWAK cannot read this layer type.").arg(label));
            continue;
        }
        const QUrl parsed(url);
        if (!parsed.isValid() || parsed.scheme() != u"https" || parsed.host().isEmpty() ||
            !parsed.userInfo().isEmpty() || url.contains(QStringLiteral("{$"))) {
            reasons.append(tr("%1: the layer needs an HTTPS URL without credentials.").arg(label));
            continue;
        }
        const int minimum = zoom(layer.value(QStringLiteral("minZoom")), 0);
        const int maximum = std::max(minimum, zoom(layer.value(QStringLiteral("maxZoom")), 19));
        auto source = mapDefaults(label, url, attribution, maximum);
        source.insert(QStringLiteral("minZoom"), minimum);
        source.insert(QStringLiteral("format"), QStringLiteral("org.swak.gdal.raster"));
        source.insert(QStringLiteral("service"), service);
        if (service == u"xyz") {
            source.insert(QStringLiteral("invertY"),
                          layer.value(QStringLiteral("invertYCoordinate")).toBool());
        } else {
            source.insert(QStringLiteral("layer"),
                          plain(layer.value(QStringLiteral("layers")).toString(), 200));
            source.insert(QStringLiteral("style"), QString{});
            source.insert(
                QStringLiteral("imageFormat"),
                layer.value(QStringLiteral("tileType")).toString().contains(QStringLiteral("jp"))
                    ? QStringLiteral("image/jpeg")
                    : QStringLiteral("image/png"));
        }
        const auto key = layer.value(QStringLiteral("uid")).toString();
        auto id =
            QStringLiteral("tak.%1.%2").arg(idPart(serverId), idPart(key.isEmpty() ? label : key));
        for (int n = 2; ids.contains(id); ++n) {
            id = QStringLiteral("tak.%1.%2-%3")
                     .arg(idPart(serverId), idPart(key.isEmpty() ? label : key))
                     .arg(n);
        }
        ids.insert(id);
        result.append({id, label, source});
    }
    *layers = result;
    if (skipped) {
        *skipped = reasons;
    }
    return true;
}
} // namespace swak

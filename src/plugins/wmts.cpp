#include "core/interfaces.h"
#include "core/maps.h"

#include <QUrlQuery>

#include <algorithm>
namespace swak {
class WmtsProvider final : public QObject, public Plugin, public MapProvider
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "wmts/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapExtension swak::MapProvider)
public:
    QString mapId() const override
    {
        return QStringLiteral("org.swak.wmts");
    }
    QString mapName() const override
    {
        return tr("WMTS 1.0 image tiles");
    }
    QJsonObject defaults() const override
    {
        auto source = mapDefaults(
            tr("NASA GIBS Blue Marble"),
            QStringLiteral("https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/wmts.cgi"),
            QStringLiteral("NASA EOSDIS GIBS | https://earthdata.nasa.gov/gibs"),
            8);
        source.insert(QStringLiteral("layer"),
                      QStringLiteral("BlueMarble_ShadedRelief_Bathymetry"));
        source.insert(QStringLiteral("matrixSet"), QStringLiteral("GoogleMapsCompatible_Level8"));
        source.insert(QStringLiteral("matrixPrefix"), QString{});
        source.insert(QStringLiteral("style"), QStringLiteral("default"));
        source.insert(QStringLiteral("time"), QString{});
        source.insert(QStringLiteral("imageFormat"), QStringLiteral("image/jpeg"));
        return source;
    }
    QList<SettingField> fields() const override
    {
        auto result = commonMapFields();
        result.append(
            {textSetting(QStringLiteral("layer"), tr("Layer identifier"), 200),
             textSetting(
                 QStringLiteral("matrixSet"), tr("Web Mercator matrix set (256-pixel tiles)"), 200),
             textSetting(
                 QStringLiteral("matrixPrefix"), tr("Matrix identifier prefix before zoom"), 100),
             textSetting(QStringLiteral("style"), tr("Style identifier"), 100),
             textSetting(QStringLiteral("time"), tr("Time dimension (optional)"), 100),
             choiceSetting(QStringLiteral("imageFormat"),
                           tr("Image format"),
                           {tr("JPEG"), tr("PNG")},
                           {QStringLiteral("image/jpeg"), QStringLiteral("image/png")})});
        return result;
    }
    QString validate(const QJsonObject &source) const override
    {
        const auto error = validateMapSource(source);
        if (!error.isEmpty()) {
            return error;
        }
        const auto parameters = fields();
        if (std::any_of(parameters.begin(), parameters.end(), [&](const auto &field) {
                return field.kind == SettingField::Kind::Text &&
                       source.value(field.path).toString().size() >
                           static_cast<qsizetype>(field.maximum);
            })) {
            return tr("A WMTS parameter is too long.");
        }
        if (source.value(QStringLiteral("layer")).toString().isEmpty() ||
            source.value(QStringLiteral("matrixSet")).toString().isEmpty()) {
            return tr("Enter the layer and Web Mercator matrix set.");
        }
        const auto format = source.value(QStringLiteral("imageFormat")).toString();
        if (format != QStringLiteral("image/png") && format != QStringLiteral("image/jpeg")) {
            return tr("Select PNG or JPEG.");
        }
        if (QUrl(source.value(QStringLiteral("endpoint")).toString()).hasQuery()) {
            return tr("Enter the service URL without query parameters.");
        }
        return {};
    }
    QUrl tileUrl(const QJsonObject &source, int z, int x, int y) const override
    {
        if (!validTile(z, x, y) || !validate(source).isEmpty()) {
            return {};
        }
        QUrl url(source.value(QStringLiteral("endpoint")).toString());
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("SERVICE"), QStringLiteral("WMTS"));
        query.addQueryItem(QStringLiteral("VERSION"), QStringLiteral("1.0.0"));
        query.addQueryItem(QStringLiteral("REQUEST"), QStringLiteral("GetTile"));
        query.addQueryItem(QStringLiteral("LAYER"),
                           source.value(QStringLiteral("layer")).toString());
        query.addQueryItem(QStringLiteral("STYLE"),
                           source.value(QStringLiteral("style")).toString());
        query.addQueryItem(QStringLiteral("TILEMATRIXSET"),
                           source.value(QStringLiteral("matrixSet")).toString());
        query.addQueryItem(QStringLiteral("TILEMATRIX"),
                           source.value(QStringLiteral("matrixPrefix")).toString() +
                               QString::number(z));
        query.addQueryItem(QStringLiteral("TILEROW"), QString::number(y));
        query.addQueryItem(QStringLiteral("TILECOL"), QString::number(x));
        query.addQueryItem(QStringLiteral("FORMAT"),
                           source.value(QStringLiteral("imageFormat")).toString());
        if (!source.value(QStringLiteral("time")).toString().isEmpty()) {
            query.addQueryItem(QStringLiteral("TIME"),
                               source.value(QStringLiteral("time")).toString());
        }
        url.setQuery(
            query.query(QUrl::FullyEncoded).replace(QLatin1Char('+'), QStringLiteral("%2B")),
            QUrl::StrictMode);
        return url;
    }
};
} // namespace swak
#include "wmts.moc"

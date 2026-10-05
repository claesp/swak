#include "core/interfaces.h"
#include "core/routing.h"

#include <QJsonDocument>

#include <cmath>
namespace swak {
class ValhallaProvider final : public QObject, public Plugin, public RouteProvider
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "valhalla/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::RouteProvider)
public:
    QString routeId() const override
    {
        return QStringLiteral("org.swak.valhalla");
    }
    QString routeName() const override
    {
        return tr("Valhalla");
    }
    QJsonObject presets() const override
    {
        return {{QStringLiteral("fossgis"),
                 QJsonObject{
                     {"name", "FOSSGIS Valhalla demo"},
                     {"endpoint", "https://valhalla1.openstreetmap.de/route"},
                     {"attribution",
                      "Routing: Valhalla / FOSSGIS. Data: OpenStreetMap contributors (ODbL)."}}}};
    }
    QJsonObject
    request(QPointF start, QPointF end, const QString &mode, double speed) const override
    {
        const QString costing = mode == QStringLiteral("driving")   ? QStringLiteral("auto")
                                : mode == QStringLiteral("cycling") ? QStringLiteral("bicycle")
                                                                    : QStringLiteral("pedestrian");
        QJsonObject options{{"shortest", true}};
        if (costing == QStringLiteral("auto")) {
            options.insert("disable_hierarchy_pruning", true);
        }
        if (costing == QStringLiteral("pedestrian")) {
            options.insert("walking_speed", mode == QStringLiteral("running") ? speed : 5.1);
        }
        return {{"locations",
                 QJsonArray{QJsonObject{{"lat", start.y()}, {"lon", start.x()}},
                            QJsonObject{{"lat", end.y()}, {"lon", end.x()}}}},
                {"costing", costing},
                {"costing_options", QJsonObject{{costing, options}}},
                {"units", "kilometers"},
                {"format", "osrm"},
                {"shape_format", "geojson"},
                {"directions_type", "none"}};
    }
    RouteResult decode(const QByteArray &body) const override
    {
        const auto invalid = [] {
            return RouteResult{
                {}, 0, 0, tr("The provider returned an invalid or unsupported route.")};
        };
        if (body.size() > 4 * 1024 * 1024) {
            return invalid();
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(body, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            return invalid();
        }
        const auto root = document.object();
        if (root.value("code").toString() != QStringLiteral("Ok")) {
            return RouteResult{
                {}, 0, 0, tr("No route is available for these points and travel mode.")};
        }
        const auto routes = root.value("routes").toArray();
        if (routes.size() != 1) {
            return invalid();
        }
        const auto route = routes[0].toObject();
        RouteResult result;
        result.meters = route.value("distance").toDouble(-1);
        result.seconds = route.value("duration").toDouble(-1);
        if (!std::isfinite(result.meters) || result.meters < 0 || result.meters > 50000000 ||
            !std::isfinite(result.seconds) || result.seconds < 0 || result.seconds > 31536000) {
            return invalid();
        }
        const auto geometry = route.value("geometry").toObject();
        const auto points = geometry.value("coordinates").toArray();
        if (geometry.value("type").toString() != QStringLiteral("LineString") ||
            points.size() < 2 || points.size() > 50000) {
            return invalid();
        }
        result.coordinates.reserve(points.size());
        for (const auto &value : points) {
            const auto pair = value.toArray();
            if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble()) {
                return invalid();
            }
            const QPointF point(pair[0].toDouble(), pair[1].toDouble());
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
                std::abs(point.x()) > 180 || std::abs(point.y()) > 85.051129) {
                return invalid();
            }
            result.coordinates.append(point);
        }
        return result;
    }
};
} // namespace swak
#include "valhalla.moc"

#include "core/gdal_support.h"
#include "core/image_allocation.h"
#include "core/interfaces.h"
#include "core/maps.h"
#include "swak/version.h"

#include <QCryptographicHash>
#include <QDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal_priv.h>
#include <gdal_utils.h>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

namespace swak {
namespace {
constexpr double MercatorEdge = 20037508.342789244;
constexpr int TileSize = 256;

QString serviceOf(const QJsonObject &source)
{
    const auto service = source.value(QStringLiteral("service")).toString();
    if (!service.isEmpty()) {
        return service;
    }
    const auto format = source.value(QStringLiteral("format")).toString();
    return format == u"org.swak.wms"    ? QStringLiteral("wms")
           : format == u"org.swak.wmts" ? QStringLiteral("wmts")
                                        : QStringLiteral("xyz");
}
bool localDataset(const QString &endpoint)
{
    return endpoint.startsWith('/') && !endpoint.startsWith(QStringLiteral("/vsi"));
}
// Returns the HTTPS URL inside a remote GDAL dataset name, or an empty URL.
QUrl remoteUrl(const QString &endpoint)
{
    for (const auto &prefix :
         {QStringLiteral("/vsicurl/"), QStringLiteral("WMTS:"), QStringLiteral("WMS:")}) {
        if (endpoint.startsWith(prefix)) {
            return QUrl(endpoint.mid(prefix.size()).section(',', 0, 0), QUrl::StrictMode);
        }
    }
    return {};
}
QString xml(const QString &text)
{
    return text.toHtmlEscaped();
}
QString withSecret(const QString &url, const RasterRequest &request)
{
    if (request.source.value(QStringLiteral("authMode")).toString() != u"query") {
        return url;
    }
    // The placeholders stay literal. QUrlQuery would encode the dollar signs.
    const auto parameter = request.source.value(QStringLiteral("authParameter")).toString();
    const auto secret =
        QString::fromLatin1(QUrl::toPercentEncoding(QString::fromLatin1(request.credential)));
    return url + (url.contains('?') ? QLatin1Char('&') : QLatin1Char('?')) + parameter + '=' +
           secret;
}
QString encoded(const QString &value, const QByteArray &keep = {})
{
    return QString::fromLatin1(QUrl::toPercentEncoding(value, keep));
}
QString wmtsUrl(const QJsonObject &source)
{
    const auto text = [&source](const char *key) {
        return encoded(source.value(QLatin1String(key)).toString());
    };
    auto url = source.value(QStringLiteral("endpoint")).toString() +
               QStringLiteral("?SERVICE=WMTS&VERSION=1.0.0&REQUEST=GetTile&LAYER=") +
               text("layer") + QStringLiteral("&STYLE=") + text("style") +
               QStringLiteral("&TILEMATRIXSET=") + text("matrixSet") + QStringLiteral("&FORMAT=") +
               text("imageFormat");
    if (!source.value(QStringLiteral("time")).toString().isEmpty()) {
        url += QStringLiteral("&TIME=") + text("time");
    }
    // GDAL fills the tile placeholders.
    return url + QStringLiteral("&TILEMATRIX=") + text("matrixPrefix") +
           QStringLiteral("${z}&TILEROW=${y}&TILECOL=${x}");
}
bool uncheckedTls(const RasterRequest &request)
{
    return request.source.value(QStringLiteral("ignoreTls")).toBool(false);
}
// Tiles fetched without certificate checks never serve a checked source. An empty result means
// no disk cache, for example for a source with credentials.
QString cacheFolder(const RasterRequest &request)
{
    if (request.cacheDirectory.isEmpty() || authenticatedMap(request.source)) {
        return {};
    }
    return QDir(request.cacheDirectory)
        .filePath(uncheckedTls(request) ? QStringLiteral("unchecked") : QStringLiteral("verified"));
}
QString cacheElement(const RasterRequest &request, const QString &name)
{
    const auto folder = cacheFolder(request);
    if (folder.isEmpty()) {
        return {};
    }
    const auto key =
        QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    return QStringLiteral("<Cache><Path>%1</Path><Expires>604800</Expires><MaxSize>%2</MaxSize>"
                          "</Cache>")
        .arg(xml(QDir(folder).filePath(QString::fromLatin1(key))))
        .arg(request.cacheBytes);
}
// Builds a GDAL WMS driver description for tile and map services.
QString serviceDescription(const RasterRequest &request)
{
    const auto &source = request.source;
    const auto service = serviceOf(source);
    const int maxZoom = source.value(QStringLiteral("maxZoom")).toInt();
    // The upper left corner is (-edge, +edge) in Web Mercator meters.
    const auto corners =
        QStringLiteral("<UpperLeftX>%1</UpperLeftX><UpperLeftY>%2</UpperLeftY>"
                       "<LowerRightX>%2</LowerRightX><LowerRightY>%1</LowerRightY>")
            .arg(-MercatorEdge, 0, 'f', 6)
            .arg(MercatorEdge, 0, 'f', 6);
    QString body;
    if (service == u"wms") {
        const auto server =
            withSecret(source.value(QStringLiteral("endpoint")).toString(), request);
        body =
            QStringLiteral("<Service name=\"WMS\"><Version>1.3.0</Version><ServerUrl>%1</ServerUrl>"
                           "<Layers>%2</Layers><Styles>%3</Styles><CRS>EPSG:3857</CRS>"
                           "<ImageFormat>%4</ImageFormat><Transparent>TRUE</Transparent>"
                           "</Service><DataWindow>%5<SizeX>%6</SizeX><SizeY>%6</SizeY>"
                           "</DataWindow>")
                .arg(xml(server),
                     // GDAL inserts these values without encoding. Keep commas as separators.
                     xml(encoded(source.value(QStringLiteral("layer")).toString(), ",")),
                     xml(encoded(source.value(QStringLiteral("style")).toString(), ",")),
                     xml(source.value(QStringLiteral("imageFormat")).toString()),
                     corners,
                     QString::number(qint64(TileSize) << maxZoom));
    } else {
        auto server = service == u"wmts" ? wmtsUrl(source)
                                         : source.value(QStringLiteral("endpoint")).toString();
        const bool quadkey = server.contains(QStringLiteral("{q}"));
        server.replace(QStringLiteral("{z}"), QStringLiteral("${z}"))
            .replace(QStringLiteral("{x}"), QStringLiteral("${x}"))
            .replace(QStringLiteral("{y}"), QStringLiteral("${y}"))
            .replace(QStringLiteral("{q}"), QStringLiteral("${quadkey}"));
        server = withSecret(server, request);
        const bool bottom = source.value(QStringLiteral("invertY")).toBool(false);
        body = QStringLiteral("<Service name=\"%1\"><ServerUrl>%2</ServerUrl></Service>"
                              "<DataWindow>%3<TileLevel>%4</TileLevel><TileCountX>1</TileCountX>"
                              "<TileCountY>1</TileCountY><YOrigin>%5</YOrigin></DataWindow>")
                   .arg(quadkey ? QStringLiteral("VirtualEarth") : QStringLiteral("TMS"),
                        xml(server),
                        corners,
                        QString::number(maxZoom),
                        bottom ? QStringLiteral("bottom") : QStringLiteral("top"));
    }
    // Concatenate here. A second arg() pass would change percent signs in the URLs.
    return QStringLiteral("<GDAL_WMS>") + body +
           QStringLiteral("<Projection>EPSG:3857</Projection><BlockSizeX>256</BlockSizeX>"
                          "<BlockSizeY>256</BlockSizeY><BandsCount>4</BandsCount>"
                          "<MaxConnections>2</MaxConnections><Timeout>10</Timeout>"
                          "<UserAgent>SWAK/" SWAK_VERSION " (native map viewer)</UserAgent>") +
           (source.value(QStringLiteral("ignoreTls")).toBool(false)
                ? QStringLiteral("<UnsafeSSL>true</UnsafeSSL>")
                : QString{}) +
           cacheElement(request, body) + QStringLiteral("</GDAL_WMS>");
}
QString datasetName(const RasterRequest &request)
{
    const auto service = serviceOf(request.source);
    return service == u"dataset" ? request.source.value(QStringLiteral("endpoint")).toString()
                                 : serviceDescription(request);
}
const char *const *allowedDrivers(const QString &name)
{
    static const char *const wms[] = {"WMS", nullptr};
    static const char *const wmts[] = {"WMTS", nullptr};
    if (name.startsWith(QStringLiteral("<GDAL_WMS>")) || name.startsWith(QStringLiteral("WMS:"))) {
        return wms;
    }
    if (name.startsWith(QStringLiteral("WMTS:"))) {
        return wmts;
    }
    return name.startsWith(QStringLiteral("/vsicurl/")) ? gdalRemoteRasterDrivers().data()
                                                        : gdalRasterDrivers().data();
}
// Sets GDAL HTTP options for this thread only and restores them on exit.
class HttpOptions final
{
public:
    explicit HttpOptions(const RasterRequest &request)
    {
        const auto &source = request.source;
        const auto mode = source.value(QStringLiteral("authMode")).toString();
        if (uncheckedTls(request)) {
            set("GDAL_HTTP_UNSAFESSL", "YES");
        }
        // WMS: and WMTS: dataset names have no Cache element. Without these options, GDAL
        // caches their tiles in its own folder, outside the SWAK limit and TLS separation.
        if (const auto folder = cacheFolder(request); folder.isEmpty()) {
            set("GDAL_ENABLE_WMS_CACHE", "NO");
        } else {
            set("GDAL_DEFAULT_WMS_CACHE_PATH",
                QDir(folder).filePath(QStringLiteral("datasets")).toUtf8());
        }
        if (mode == u"basic") {
            set("GDAL_HTTP_AUTH", "BASIC");
            set("GDAL_HTTP_USERPWD",
                source.value(QStringLiteral("authUser")).toString().toUtf8() + ':' +
                    request.credential);
        } else if (mode == u"bearer") {
            set("GDAL_HTTP_HEADERS", "Authorization: Bearer " + request.credential);
        } else if (mode == u"header") {
            set("GDAL_HTTP_HEADERS",
                source.value(QStringLiteral("authParameter")).toString().toLatin1() + ": " +
                    request.credential);
        }
    }
    ~HttpOptions()
    {
        for (const auto &key : keys_) {
            CPLSetThreadLocalConfigOption(key.constData(), nullptr);
        }
    }
    HttpOptions(const HttpOptions &) = delete;
    HttpOptions &operator=(const HttpOptions &) = delete;

private:
    void set(const char *key, const QByteArray &value)
    {
        CPLSetThreadLocalConfigOption(key, value.constData());
        keys_.append(QByteArray(key));
    }
    QList<QByteArray> keys_;
};
struct OpenDataset
{
    QString name;
    GdalDataset dataset;
    // Value range for bands that are not 8-bit.
    double minimum = 0;
    double maximum = 255;
};
// Keeps a few open datasets for reuse. A worker takes a dataset, uses it alone, and returns it.
// Datasets close on ordinary threads or at shutdown, never in a thread-exit destructor. GDAL
// would then create a PROJ context for the ending thread that nothing frees.
class DatasetPool final
{
public:
    std::unique_ptr<OpenDataset> take(const QString &name)
    {
        const std::lock_guard lock(mutex_);
        const auto found = std::find_if(idle_.begin(), idle_.end(), [&name](const auto &item) {
            return item->name == name;
        });
        if (found == idle_.end()) {
            return nullptr;
        }
        auto result = std::move(*found);
        idle_.erase(found);
        return result;
    }
    void give(std::unique_ptr<OpenDataset> dataset)
    {
        // Close the oldest dataset outside the lock. Closing can be slow.
        auto evicted = store(std::move(dataset));
        evicted.reset();
    }
    void clear()
    {
        auto closing = takeAll();
        closing.clear();
    }

private:
    std::unique_ptr<OpenDataset> store(std::unique_ptr<OpenDataset> dataset)
    {
        const std::lock_guard lock(mutex_);
        idle_.push_back(std::move(dataset));
        if (idle_.size() <= 8) {
            return nullptr;
        }
        auto oldest = std::move(idle_.front());
        idle_.erase(idle_.begin());
        return oldest;
    }
    std::vector<std::unique_ptr<OpenDataset>> takeAll()
    {
        const std::lock_guard lock(mutex_);
        return std::exchange(idle_, {});
    }
    std::mutex mutex_;
    std::vector<std::unique_ptr<OpenDataset>> idle_;
};
DatasetPool &datasetPool()
{
    static DatasetPool pool;
    static std::once_flag registered;
    std::call_once(registered, [] {
        addGdalShutdownTask([] {
            datasetPool().clear();
        });
    });
    return pool;
}
// The key names a pooled dataset. It holds the certificate policy, because GDAL keeps that
// policy for the life of an open dataset.
std::unique_ptr<OpenDataset> openDataset(const QString &name, const QString &key, QString &error)
{
    if (auto reused = datasetPool().take(key)) {
        return reused;
    }
    CPLErrorReset();
    GdalDataset dataset(GDALDataset::FromHandle(GDALOpenEx(name.toUtf8().constData(),
                                                           GDAL_OF_RASTER | GDAL_OF_READONLY,
                                                           allowedDrivers(name),
                                                           nullptr,
                                                           nullptr)));
    if (!dataset || dataset->GetRasterCount() < 1) {
        error = gdalError(QObject::tr("The map dataset cannot be opened."));
        return nullptr;
    }
    auto opened = std::make_unique<OpenDataset>();
    opened->name = key;
    auto *band = dataset->GetRasterBand(1);
    if (band->GetRasterDataType() != GDT_Byte) {
        double range[2] = {0, 255};
        if (GDALComputeRasterMinMax(GDALRasterBand::ToHandle(band), TRUE, range) == CE_None &&
            std::isfinite(range[0]) && std::isfinite(range[1]) && range[1] > range[0]) {
            opened->minimum = range[0];
            opened->maximum = range[1];
        }
    }
    opened->dataset = std::move(dataset);
    return opened;
}
int cancelWarp(double, const char *, void *data)
{
    const auto *flag = *static_cast<const std::atomic_bool **>(data);
    return flag->load(std::memory_order_relaxed) ? FALSE : TRUE;
}
QRgb pixel(const std::array<double, 4> &channels, bool gray)
{
    const auto byte = [](double value) {
        return std::isfinite(value) ? static_cast<int>(std::clamp(std::lround(value), 0L, 255L))
                                    : 0;
    };
    if (gray) {
        return qRgba(byte(channels[0]), byte(channels[0]), byte(channels[0]), byte(channels[3]));
    }
    return qRgba(byte(channels[0]), byte(channels[1]), byte(channels[2]), byte(channels[3]));
}
// Reads a window of the dataset into a 256-pixel tile and converts it to colors.
RasterTile toImage(GDALDataset &dataset,
                   const OpenDataset &range,
                   const GDALColorTable *palette,
                   int xOffset,
                   int yOffset,
                   int size)
{
    RasterTile result;
    const int bands = dataset.GetRasterCount();
    std::vector<std::vector<double>> values(static_cast<size_t>(bands));
    std::array<int, 4> roles{-1, -1, -1, -1};
    for (int i = 0; i < bands; ++i) {
        auto *band = dataset.GetRasterBand(i + 1);
        switch (band->GetColorInterpretation()) {
        case GCI_RedBand:
            roles[0] = i;
            break;
        case GCI_GreenBand:
            roles[1] = i;
            break;
        case GCI_BlueBand:
            roles[2] = i;
            break;
        case GCI_AlphaBand:
            roles[3] = i;
            break;
        case GCI_GrayIndex:
        case GCI_PaletteIndex:
            roles[0] = i;
            break;
        default:
            if (i < 3 && roles[size_t(i)] < 0) {
                roles[size_t(i)] = i;
            }
            break;
        }
        values[static_cast<size_t>(i)].resize(TileSize * TileSize);
        if (band->RasterIO(GF_Read,
                           xOffset,
                           yOffset,
                           size,
                           size,
                           values[static_cast<size_t>(i)].data(),
                           TileSize,
                           TileSize,
                           GDT_Float64,
                           0,
                           0,
                           nullptr) != CE_None) {
            result.error = gdalError(QObject::tr("The map tile cannot be read."));
            return result;
        }
    }
    const bool gray = roles[1] < 0 || roles[2] < 0;
    const bool scaled = dataset.GetRasterBand(1)->GetRasterDataType() != GDT_Byte;
    const double stretch = scaled ? 255 / (range.maximum - range.minimum) : 1;
    auto image = allocateImage(TileSize, TileSize, QImage::Format_ARGB32);
    if (image.isNull()) {
        result.error = QObject::tr("Map tile image allocation failed.");
        return result;
    }
    for (int y = 0; y < TileSize; ++y) {
        for (int x = 0; x < TileSize; ++x) {
            const auto offset = static_cast<size_t>(y * TileSize + x);
            std::array<double, 4> channels{0, 0, 0, 255};
            for (size_t c = 0; c < 4; ++c) {
                if (roles[c] >= 0) {
                    channels[c] = values[static_cast<size_t>(roles[c])][offset];
                }
            }
            if (palette) {
                // A float band with a palette can hold NaN or a large value. Only a finite index
                // inside the table converts to int.
                const double index = channels[0];
                const auto *entry = index >= 0 && index < palette->GetColorEntryCount()
                                        ? palette->GetColorEntry(static_cast<int>(index))
                                        : nullptr;
                channels = entry ? std::array<double, 4>{double(entry->c1),
                                                         double(entry->c2),
                                                         double(entry->c3),
                                                         std::min(channels[3], double(entry->c4))}
                                 : std::array<double, 4>{0, 0, 0, 0};
                image.setPixel(x, y, pixel(channels, false));
                continue;
            }
            if (scaled) {
                for (size_t c = 0; c < 3; ++c) {
                    channels[c] = (channels[c] - range.minimum) * stretch;
                }
            }
            image.setPixel(x, y, pixel(channels, gray));
        }
    }
    result.image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return result;
}
RasterTile
warp(OpenDataset &opened, const RasterRequest &request, const std::atomic_bool &cancelled)
{
    RasterTile result;
    auto *source = opened.dataset.get();
    const auto *palette = source->GetRasterBand(1)->GetColorTable();
    const double span = 2 * MercatorEdge / double(1 << request.zoom);
    const double minX = -MercatorEdge + request.x * span;
    const double maxY = MercatorEdge - request.y * span;
    const QList<QByteArray> arguments{"-of",
                                      "MEM",
                                      "-t_srs",
                                      "EPSG:3857",
                                      "-te",
                                      QByteArray::number(minX, 'f', 6),
                                      QByteArray::number(maxY - span, 'f', 6),
                                      QByteArray::number(minX + span, 'f', 6),
                                      QByteArray::number(maxY, 'f', 6),
                                      "-ts",
                                      QByteArray::number(TileSize),
                                      QByteArray::number(TileSize),
                                      "-r",
                                      palette ? "near" : "bilinear",
                                      "-dstalpha",
                                      "-wm",
                                      "64"};
    CPLStringList argv;
    for (const auto &argument : arguments) {
        argv.AddString(argument.constData());
    }
    std::unique_ptr<GDALWarpAppOptions, decltype(&GDALWarpAppOptionsFree)> warpOptions(
        GDALWarpAppOptionsNew(argv.List(), nullptr), GDALWarpAppOptionsFree);
    if (!warpOptions) {
        result.error = QObject::tr("The map tile options are invalid.");
        return result;
    }
    const std::atomic_bool *flag = &cancelled;
    GDALWarpAppOptionsSetProgress(warpOptions.get(), cancelWarp, &flag);
    GDALDatasetH input = GDALDataset::ToHandle(source);
    int usageError = FALSE;
    CPLErrorReset();
    GdalDataset tile(
        GDALDataset::FromHandle(GDALWarp("", nullptr, 1, &input, warpOptions.get(), &usageError)));
    if (!tile || usageError || cancelled) {
        result.error = cancelled ? QObject::tr("Map tile request cancelled.")
                                 : gdalError(QObject::tr("The map tile cannot be rendered."));
        return result;
    }
    return toImage(*tile, opened, palette, 0, 0, TileSize);
}
RasterTile render(const RasterRequest &request, const std::atomic_bool &cancelled)
{
    RasterTile result;
    if (!validTile(request.zoom, request.x, request.y) || cancelled) {
        result.error = QObject::tr("Map tile request cancelled.");
        return result;
    }
    initializeGdal();
    const HttpOptions options(request);
    const auto name = datasetName(request);
    auto opened = openDataset(
        name, uncheckedTls(request) ? name + QStringLiteral("\n#unchecked") : name, result.error);
    if (!opened) {
        result.providerWide = true;
        return result;
    }
    // Return the dataset for reuse when this tile is complete.
    struct Return
    {
        std::unique_ptr<OpenDataset> &dataset;
        ~Return()
        {
            datasetPool().give(std::move(dataset));
        }
    } giveBack{opened};
    if (serviceOf(request.source) == u"dataset") {
        return warp(*opened, request, cancelled);
    }
    // Service tiles already use the Web Mercator grid. Read the exact window so that GDAL
    // fetches one remote tile and no neighbors.
    const int maxZoom = request.source.value(QStringLiteral("maxZoom")).toInt();
    if (request.zoom > maxZoom) {
        result.error = QObject::tr("The map tile is beyond the provider zoom range.");
        return result;
    }
    const int size = TileSize << (maxZoom - request.zoom);
    return toImage(*opened->dataset, *opened, nullptr, request.x * size, request.y * size, size);
}
} // namespace
class GdalRasterProvider final : public QObject,
                                 public Plugin,
                                 public MapProvider,
                                 public RasterTileProvider
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Plugin/2.0" FILE "gdal_raster/MANIFEST")
    Q_INTERFACES(swak::Plugin swak::MapExtension swak::MapProvider swak::RasterTileProvider)
public:
    QString mapId() const override
    {
        return QStringLiteral("org.swak.gdal.raster");
    }
    QString mapName() const override
    {
        return tr("GDAL raster maps");
    }
    QStringList legacyIds() const override
    {
        return {QStringLiteral("org.swak.xyz"),
                QStringLiteral("org.swak.wms"),
                QStringLiteral("org.swak.wmts")};
    }
    QJsonObject migrate(const QJsonObject &source) const override
    {
        auto result = source;
        result.insert(QStringLiteral("service"), serviceOf(source));
        return result;
    }
    QJsonObject defaults() const override
    {
        auto source = mapDefaults(
            tr("OpenStreetMap"),
            QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
            QStringLiteral(
                "© OpenStreetMap contributors | https://www.openstreetmap.org/copyright"),
            19);
        source.insert(QStringLiteral("service"), QStringLiteral("xyz"));
        source.insert(QStringLiteral("invertY"), false);
        return source;
    }
    QJsonObject additionalSources() const override
    {
        QJsonObject result;
        const auto xyz = [&result](const QString &id, QJsonObject source) {
            source.insert(QStringLiteral("service"), QStringLiteral("xyz"));
            result.insert(id, source);
        };
        for (const auto &[id, name, service, zoom] :
             {std::tuple{QStringLiteral("org.swak.xyz.usgs-topo"),
                         tr("USGS Topo"),
                         QStringLiteral("USGSTopo"),
                         8},
              std::tuple{QStringLiteral("org.swak.xyz.usgs-imagery"),
                         tr("USGS Imagery"),
                         QStringLiteral("USGSImageryOnly"),
                         8},
              std::tuple{QStringLiteral("org.swak.xyz.usgs-imagery-topo"),
                         tr("USGS Imagery Topo"),
                         QStringLiteral("USGSImageryTopo"),
                         8},
              std::tuple{QStringLiteral("org.swak.xyz.usgs-relief"),
                         tr("USGS Shaded Relief"),
                         QStringLiteral("USGSShadedReliefOnly"),
                         7}}) {
            const auto root =
                QStringLiteral("https://basemap.nationalmap.gov/arcgis/rest/services/") + service +
                QStringLiteral("/MapServer");
            auto source =
                mapDefaults(name,
                            root + QStringLiteral("/tile/{z}/{y}/{x}"),
                            QStringLiteral("USGS The National Map and contributors | ") + root,
                            zoom);
            if (id == u"org.swak.xyz.usgs-imagery-topo") {
                source.insert(QStringLiteral("minZoom"), 1);
            }
            xyz(id, source);
        }
        for (const auto &style : {QStringLiteral("outdoors"), QStringLiteral("cycle")}) {
            auto source = mapDefaults(
                style == u"outdoors" ? tr("Thunderforest Outdoors (API key)")
                                     : tr("Thunderforest OpenCycleMap (API key)"),
                QStringLiteral("https://api.thunderforest.com/%1/{z}/{x}/{y}.png").arg(style),
                QStringLiteral(
                    "Maps © Thunderforest, Data © OpenStreetMap contributors | "
                    "https://www.thunderforest.com/ | https://www.openstreetmap.org/copyright"),
                19);
            source.insert(QStringLiteral("authMode"), QStringLiteral("query"));
            source.insert(QStringLiteral("authParameter"), QStringLiteral("apikey"));
            source.insert(QStringLiteral("authEnvironment"),
                          QStringLiteral("SWAK_MAP_THUNDERFOREST_KEY"));
            xyz(QStringLiteral("org.swak.xyz.thunderforest-") + style, source);
        }
        auto wms = mapDefaults(tr("terrestris OpenStreetMap"),
                               QStringLiteral("https://ows.terrestris.de/osm/service"),
                               QStringLiteral("© OpenStreetMap contributors | terrestris | "
                                              "https://www.openstreetmap.org/copyright"),
                               18);
        wms.insert(QStringLiteral("service"), QStringLiteral("wms"));
        wms.insert(QStringLiteral("layer"), QStringLiteral("OSM-WMS"));
        wms.insert(QStringLiteral("style"), QString{});
        wms.insert(QStringLiteral("imageFormat"), QStringLiteral("image/png"));
        result.insert(QStringLiteral("org.swak.wms"), wms);
        auto wmts = mapDefaults(
            tr("NASA GIBS Blue Marble"),
            QStringLiteral("https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/wmts.cgi"),
            QStringLiteral("NASA EOSDIS GIBS | https://earthdata.nasa.gov/gibs"),
            8);
        wmts.insert(QStringLiteral("service"), QStringLiteral("wmts"));
        wmts.insert(QStringLiteral("layer"), QStringLiteral("BlueMarble_ShadedRelief_Bathymetry"));
        wmts.insert(QStringLiteral("matrixSet"), QStringLiteral("GoogleMapsCompatible_Level8"));
        wmts.insert(QStringLiteral("matrixPrefix"), QString{});
        wmts.insert(QStringLiteral("style"), QStringLiteral("default"));
        wmts.insert(QStringLiteral("time"), QString{});
        wmts.insert(QStringLiteral("imageFormat"), QStringLiteral("image/jpeg"));
        result.insert(QStringLiteral("org.swak.wmts"), wmts);
        // Satellite imagery and night lights that suit the Modern display mode.
        auto esri = mapDefaults(
            tr("Esri World Imagery"),
            QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/"
                           "MapServer/tile/{z}/{y}/{x}"),
            QStringLiteral("Powered by Esri | Source: Esri, Maxar, Earthstar Geographics, and the "
                           "GIS User Community | "
                           "https://www.esri.com/en-us/legal/terms/full-master-agreement"),
            19);
        xyz(QStringLiteral("org.swak.xyz.esri-world-imagery"), esri);
        const auto gibs = [&](const QString &id,
                              const QString &name,
                              const QString &layer,
                              const QString &matrixSet,
                              const QString &format,
                              const QString &time,
                              int maxZoom) {
            auto source = mapDefaults(
                name,
                QStringLiteral("https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/wmts.cgi"),
                QStringLiteral("NASA EOSDIS GIBS | https://earthdata.nasa.gov/gibs"),
                maxZoom);
            source.insert(QStringLiteral("service"), QStringLiteral("wmts"));
            source.insert(QStringLiteral("layer"), layer);
            source.insert(QStringLiteral("matrixSet"), matrixSet);
            source.insert(QStringLiteral("matrixPrefix"), QString{});
            source.insert(QStringLiteral("style"), QStringLiteral("default"));
            source.insert(QStringLiteral("time"), time);
            source.insert(QStringLiteral("imageFormat"), format);
            result.insert(id, source);
        };
        // Without a time, GIBS serves its default day, which is the newest one.
        gibs(QStringLiteral("org.swak.wmts.gibs-viirs-true-color"),
             tr("NASA GIBS VIIRS True Color (daily)"),
             QStringLiteral("VIIRS_SNPP_CorrectedReflectance_TrueColor"),
             QStringLiteral("GoogleMapsCompatible_Level9"),
             QStringLiteral("image/jpeg"),
             QString{},
             9);
        gibs(QStringLiteral("org.swak.wmts.gibs-black-marble"),
             tr("NASA GIBS Black Marble (night lights)"),
             QStringLiteral("VIIRS_Black_Marble"),
             QStringLiteral("GoogleMapsCompatible_Level8"),
             QStringLiteral("image/png"),
             QStringLiteral("2016-01-01"),
             8);
        return result;
    }
    QList<SettingField> fields() const override
    {
        auto result = commonMapFields();
        for (auto &field : result) {
            if (field.path == u"endpoint") {
                field.label = tr("Location (URL template, service URL, file path, or GDAL "
                                 "dataset)");
            }
        }
        result.prepend(choiceSetting(QStringLiteral("service"),
                                     tr("Source type"),
                                     {tr("XYZ tile template"),
                                      tr("WMS 1.3 service"),
                                      tr("WMTS 1.0 KVP service"),
                                      tr("GDAL dataset or local file")},
                                     {QStringLiteral("xyz"),
                                      QStringLiteral("wms"),
                                      QStringLiteral("wmts"),
                                      QStringLiteral("dataset")}));
        const auto only = [](SettingField field, const QJsonValue &services) {
            field.enabledBy = QStringLiteral("service");
            field.enabledValue = services;
            return field;
        };
        const QJsonArray services{QStringLiteral("wms"), QStringLiteral("wmts")};
        result.append(
            {only(booleanSetting(QStringLiteral("invertY"), tr("Invert tile rows (TMS)")),
                  QStringLiteral("xyz")),
             only(textSetting(QStringLiteral("layer"), tr("Layer names or identifier"), 200),
                  services),
             only(textSetting(QStringLiteral("style"), tr("Style names or identifier"), 100),
                  services),
             only(choiceSetting(QStringLiteral("imageFormat"),
                                tr("Image format"),
                                {tr("JPEG"), tr("PNG")},
                                {QStringLiteral("image/jpeg"), QStringLiteral("image/png")}),
                  services),
             only(textSetting(QStringLiteral("matrixSet"),
                              tr("Web Mercator matrix set (256-pixel tiles)"),
                              200),
                  QStringLiteral("wmts")),
             only(textSetting(QStringLiteral("matrixPrefix"),
                              tr("Matrix identifier prefix before zoom"),
                              100),
                  QStringLiteral("wmts")),
             only(textSetting(QStringLiteral("time"), tr("Time dimension (optional)"), 100),
                  QStringLiteral("wmts"))});
        return result;
    }
    QString validate(const QJsonObject &source) const override
    {
        auto error = validateMapText(source);
        if (!error.isEmpty()) {
            return error;
        }
        const auto service = serviceOf(source);
        const auto endpoint = source.value(QStringLiteral("endpoint")).toString();
        if (service == u"dataset") {
            return validateDataset(source, endpoint);
        }
        if (service != u"xyz" && service != u"wms" && service != u"wmts") {
            return tr("Select a source type.");
        }
        const QUrl url(endpoint);
        if (!url.isValid() || url.scheme() != u"https" || url.host().isEmpty() ||
            !url.userInfo().isEmpty() || url.hasFragment()) {
            return tr("Use an HTTPS URL without credentials or a fragment.");
        }
        if (service == u"xyz") {
            return validateTemplate(source, endpoint);
        }
        if (url.hasQuery()) {
            return tr("Enter the service URL without query parameters.");
        }
        const auto layer = source.value(QStringLiteral("layer")).toString();
        if (layer.isEmpty() || layer.size() > 200 ||
            source.value(QStringLiteral("style")).toString().size() > 100) {
            return tr("Enter bounded layer and style names.");
        }
        const auto format = source.value(QStringLiteral("imageFormat")).toString();
        if (format != u"image/png" && format != u"image/jpeg") {
            return tr("Select PNG or JPEG.");
        }
        if (service == u"wmts" &&
            (source.value(QStringLiteral("matrixSet")).toString().isEmpty() ||
             source.value(QStringLiteral("matrixSet")).toString().size() > 200 ||
             source.value(QStringLiteral("matrixPrefix")).toString().size() > 100 ||
             source.value(QStringLiteral("time")).toString().size() > 100)) {
            return tr("Enter a bounded layer and matrix set.");
        }
        return {};
    }
    QUrl tileUrl(const QJsonObject &, int, int, int) const override
    {
        // GDAL fetches or reads the tiles. See renderTile.
        return {};
    }
    RasterTile renderTile(const RasterRequest &request,
                          const std::atomic_bool &cancelled) const override
    {
        if (!validate(request.source).isEmpty()) {
            RasterTile result;
            result.error = tr("The map provider is invalid.");
            result.providerWide = true;
            return result;
        }
        return render(request, cancelled);
    }

private:
    static QString validateTemplate(const QJsonObject &source, QString url)
    {
        if (source.contains(QStringLiteral("invertY")) &&
            !source.value(QStringLiteral("invertY")).isBool()) {
            return tr("The inverted row value must be true or false.");
        }
        const QStringList parts =
            url.contains(QStringLiteral("{q}"))
                ? QStringList{QStringLiteral("{q}")}
                : QStringList{QStringLiteral("{z}"), QStringLiteral("{x}"), QStringLiteral("{y}")};
        for (const auto &part : parts) {
            if (!url.contains(part)) {
                return tr("The tile URL needs %1.").arg(parts.join(QStringLiteral(", ")));
            }
            url.replace(part, QString{});
        }
        if (url.contains('{') || url.contains('}') || url.contains('$')) {
            return tr("The tile URL contains an unsupported placeholder.");
        }
        return {};
    }
    static QString validateDataset(const QJsonObject &source, const QString &endpoint)
    {
        if (endpoint.contains('<') || endpoint.contains('>')) {
            return tr("Enter a file path or a GDAL dataset name, not an XML description.");
        }
        if (localDataset(endpoint)) {
            if (endpoint.split('/').contains(QStringLiteral("..")) ||
                !QDir::isAbsolutePath(endpoint)) {
                return tr("Use an absolute local file path.");
            }
            if (authenticatedMap(source)) {
                return tr("Local files do not use credentials.");
            }
            return {};
        }
        const auto url = remoteUrl(endpoint);
        if (!url.isValid() || url.scheme() != u"https" || url.host().isEmpty() ||
            !url.userInfo().isEmpty() || url.hasFragment()) {
            return tr("Use a local file path, or /vsicurl/, WMTS:, or WMS: followed by an HTTPS "
                      "URL.");
        }
        // GDAL sends credentials to every host that a capabilities document names, and the
        // map cannot limit it to the dataset host.
        if (authenticatedMap(source)) {
            return tr("Remote GDAL datasets do not use credentials. Use a WMS, WMTS, or tile "
                      "service source instead.");
        }
        return {};
    }
};
} // namespace swak
#include "gdal_raster.moc"

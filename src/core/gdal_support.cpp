#include "core/gdal_support.h"

#include "core/logging.h"

#include <QRegularExpression>

#include <algorithm>
#include <atomic>
#include <cpl_conv.h>
#include <cpl_error.h>
#include <cpl_http.h>
#include <cpl_vsi.h>
#include <cstring>
#include <gdal.h>
#include <gdal_priv.h>
#include <mutex>
#include <ogr_srs_api.h>
#include <utility>
#include <vector>

namespace swak {
namespace {
void routeGdalError(CPLErr level, CPLErrorNum, const char *message)
{
    if (level == CE_None || level == CE_Debug) {
        return;
    }
    const auto text = redactQueryValues(QString::fromUtf8(message ? message : "")).left(512);
    logMessage(level == CE_Warning ? Severity::Debug : Severity::Warning,
               Urgency::Routine,
               QStringLiteral("swak.gdal"),
               text);
}
} // namespace
void initializeGdal()
{
    static std::once_flag once;
    std::call_once(once, [] {
        // Sidecar files would write next to user data. Directory scans can touch the network.
        CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");
        CPLSetConfigOption("GDAL_DISABLE_READDIR_ON_OPEN", "EMPTY_DIR");
        CPLSetConfigOption("GDAL_VRT_ENABLE_PYTHON", "NO");
        // Bound the raster block cache and the size of one JSON object.
        CPLSetConfigOption("GDAL_CACHEMAX", "64");
        CPLSetConfigOption("OGR_GEOJSON_MAX_OBJ_SIZE", "16");
        CPLSetConfigOption("GDAL_MAX_DATASET_POOL_SIZE", "32");
        // Network drivers keep certificate checks and use short limits.
        CPLSetConfigOption("GDAL_HTTP_UNSAFESSL", "NO");
        CPLSetConfigOption("GDAL_HTTP_TIMEOUT", "20");
        CPLSetConfigOption("GDAL_HTTP_CONNECTTIMEOUT", "10");
        CPLSetConfigOption("GDAL_HTTP_MAX_RETRY", "1");
        CPLSetConfigOption("GDAL_HTTP_USERAGENT", "SWAK/0.1 GDAL client");
        CPLSetConfigOption("CPL_VSIL_CURL_CACHE_SIZE", "16777216");
        // GML would otherwise download the WFS schema that a document names.
        CPLSetConfigOption("GML_DOWNLOAD_WFS_SCHEMA", "NO");
        // These drivers run programs, open database connections, or fetch any URL.
        CPLSetConfigOption("GDAL_SKIP",
                           "GPSBabel HTTP PG PostGISRaster MySQL ODBC PGeo MSSQLSpatial OGDI "
                           "Elastic Carto AmigoCloud NGW PLSCENES PLMOSAIC EEDA EEDAI DAAS CSW "
                           "GMLAS OGCAPI STACIT");
        CPLSetErrorHandler(routeGdalError);
        GDALAllRegister();
    });
}
namespace {
CPLHTTPResult *refuseFetch(
    const char *, CSLConstList, GDALProgressFunc, void *, CPLHTTPFetchWriteFunc, void *, void *)
{
    auto *result = static_cast<CPLHTTPResult *>(CPLCalloc(1, sizeof(CPLHTTPResult)));
    result->nStatus = 1;
    result->pszErrBuf = CPLStrdup("SWAK does not fetch URLs from data documents.");
    return result;
}
} // namespace
GdalNetworkBlock::GdalNetworkBlock() : pushed_(CPLHTTPPushFetchCallback(refuseFetch, nullptr) != 0)
{
}
GdalNetworkBlock::~GdalNetworkBlock()
{
    if (pushed_) {
        CPLHTTPPopFetchCallback();
    }
}
namespace {
std::mutex shutdownMutex;
std::vector<std::function<void()>> &shutdownTasks()
{
    static std::vector<std::function<void()>> tasks;
    return tasks;
}
} // namespace
void addGdalShutdownTask(std::function<void()> task)
{
    const std::lock_guard lock(shutdownMutex);
    shutdownTasks().push_back(std::move(task));
}
void shutdownGdal()
{
    std::vector<std::function<void()>> tasks;
    {
        const std::lock_guard lock(shutdownMutex);
        tasks.swap(shutdownTasks());
    }
    for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
        (*it)();
    }
    // This frees the PROJ context of this thread and the cached reference systems.
    OSRCleanup();
}
QString gdalVersion()
{
    return QString::fromLatin1(GDALVersionInfo("RELEASE_NAME"));
}
QString gdalBuildValue(const QString &key)
{
    const auto lines = QString::fromLatin1(GDALVersionInfo("BUILD_INFO")).split('\n');
    const auto prefix = key + QLatin1Char('=');
    const auto found = std::find_if(lines.cbegin(), lines.cend(), [&prefix](const QString &line) {
        return line.startsWith(prefix);
    });
    return found == lines.cend() ? QString{} : found->mid(prefix.size()).trimmed().left(64);
}
const std::vector<const char *> &gdalVectorDrivers()
{
    static const std::vector<const char *> drivers{
        "LIBKML",     "KML",         "GeoJSON",      "GeoJSONSeq",
        "ESRIJSON",   "TopoJSON",    "JSONFG",       "GPX",
        "GML",        "GeoRSS",      "GPKG",         "SQLite",
        "FlatGeobuf", "OpenFileGDB", "MapInfo File", "ESRI Shapefile",
        "CSV",        "S57",         "OSM",          "DXF",
        "DGN",        "JML",         "GTFS",         "PMTiles",
        "MVT",        "ODS",         "XLSX",         nullptr};
    return drivers;
}
const std::vector<const char *> &gdalRasterDrivers()
{
    static const std::vector<const char *> drivers{
        "GTiff", "COG",  "MBTiles", "GPKG", "PMTiles", "JP2OpenJPEG", "PNG",    "JPEG",
        "WEBP",  "GIF",  "BMP",     "DTED", "SRTMHGT", "NITF",        "RPFTOC", "ADRG",
        "HFA",   "ENVI", "netCDF",  "HDF5", "AAIGrid", "USGSDEM",     "BSB",    "ESRIC",
        "MRF",   "Zarr", "PDF",     "GRIB", "VRT",     nullptr};
    return drivers;
}
const std::vector<const char *> &gdalRemoteRasterDrivers()
{
    static const std::vector<const char *> drivers{"GTiff",
                                                   "COG",
                                                   "MBTiles",
                                                   "GPKG",
                                                   "PMTiles",
                                                   "JP2OpenJPEG",
                                                   "PNG",
                                                   "JPEG",
                                                   "WEBP",
                                                   "GIF",
                                                   "NITF",
                                                   "DTED",
                                                   "netCDF",
                                                   "Zarr",
                                                   nullptr};
    return drivers;
}
QString redactQueryValues(const QString &text)
{
    static const QRegularExpression query(
        QStringLiteral("([?&][A-Za-z0-9_.~-]{1,64}=)[^&\\s\"'<>]+"));
    return QString(text).replace(query, QStringLiteral("\\1***"));
}
bool declaresXmlDocumentType(const QByteArray &bytes)
{
    // A document type declaration appears before the root element.
    const auto head = bytes.left(4096);
    return head.contains("<!DOCTYPE") || head.contains("<!ENTITY");
}
QString gdalError(const QString &fallback)
{
    const char *message = CPLGetLastErrorMsg();
    const auto text =
        redactQueryValues(QString::fromUtf8(message ? message : "")).simplified().left(300);
    CPLErrorReset();
    return text.isEmpty() ? fallback : fallback + QStringLiteral(" ") + text;
}
void GdalDatasetClose::operator()(GDALDataset *dataset) const noexcept
{
    if (dataset) {
        GDALClose(GDALDataset::ToHandle(dataset));
    }
}
namespace {
QByteArray memoryPath(const QString &suffix)
{
    static std::atomic<quint64> counter = 0;
    return QByteArrayLiteral("/vsimem/swak/") + QByteArray::number(++counter) + '.' +
           suffix.toLatin1();
}
} // namespace
GdalMemoryFile::GdalMemoryFile(const QByteArray &bytes, const QString &suffix)
    : path_(memoryPath(suffix))
{
    if (bytes.isEmpty()) {
        return;
    }
    const auto size = static_cast<size_t>(bytes.size());
    // GDAL takes ownership of this buffer and frees it when the file is removed.
    auto *buffer = static_cast<GByte *>(VSIMalloc(size));
    if (!buffer) {
        return;
    }
    std::memcpy(buffer, bytes.constData(), size);
    VSILFILE *file = VSIFileFromMemBuffer(path_.constData(), buffer, size, TRUE);
    if (!file) {
        VSIFree(buffer);
        return;
    }
    VSIFCloseL(file);
    valid_ = true;
}
GdalMemoryFile::~GdalMemoryFile()
{
    if (valid_) {
        VSIUnlink(path_.constData());
    }
}
} // namespace swak

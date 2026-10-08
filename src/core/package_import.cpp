#include "core/package_import.h"

#include "core/cot.h"
#include "core/data.h"
#include "core/maps.h"
#include "core/tak_shapes.h"
#include "core/user_objects.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

namespace swak {
namespace {
// Vector formats that the GDAL vector provider reads from a local file. A shapefile
// registers through its .shp part, and the other parts stay beside it.
const QStringList VectorSuffixes{QStringLiteral("kml"),
                                 QStringLiteral("kmz"),
                                 QStringLiteral("geojson"),
                                 QStringLiteral("json"),
                                 QStringLiteral("gpx"),
                                 QStringLiteral("gml"),
                                 QStringLiteral("gpkg"),
                                 QStringLiteral("fgb"),
                                 QStringLiteral("csv"),
                                 QStringLiteral("shp")};
// Raster formats that the GDAL raster provider shows as a map. SWAK does not register VRT
// files, because they can name other files on this computer.
const QStringList RasterSuffixes{QStringLiteral("tif"), QStringLiteral("tiff")};
const QStringList ShapefileParts{
    QStringLiteral("dbf"), QStringLiteral("shx"), QStringLiteral("prj"), QStringLiteral("cpg")};

QString tr(const char *text, const char *disambiguation = nullptr, int count = -1)
{
    return QCoreApplication::translate("PackageImport", text, disambiguation, count);
}
bool cotFile(const PackageEntry &entry, const QString &suffix)
{
    return suffix == u"cot" || (suffix == u"xml" && entry.bytes.left(512).contains("<event") &&
                                entry.bytes.size() <= MaxMessageBytes);
}
QString writeFile(const QString &folder, const PackageEntry &entry)
{
    const QFileInfo info(QDir(folder).filePath(entry.path));
    if (!QDir().mkpath(info.absolutePath())) {
        return tr("Cannot create the folder for %1.").arg(entry.path);
    }
    QSaveFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(entry.bytes) != entry.bytes.size() || !file.commit()) {
        return tr("Cannot save %1.").arg(entry.path);
    }
    return {};
}
} // namespace

QString PackageImportResult::summary() const
{
    QStringList parts;
    if (objects > 0) {
        parts.append(tr("%n objects", nullptr, objects));
    }
    if (!feeds.isEmpty()) {
        parts.append(tr("%n data feeds", nullptr, static_cast<int>(feeds.size())));
    }
    if (!maps.isEmpty()) {
        parts.append(tr("%n maps", nullptr, static_cast<int>(maps.size())));
    }
    auto text = parts.isEmpty() ? tr("Nothing imported.")
                                : tr("Imported %1.").arg(parts.join(QStringLiteral(", ")));
    if (!skipped.isEmpty()) {
        text +=
            QLatin1Char(' ') + tr("%n files skipped.", nullptr, static_cast<int>(skipped.size()));
    }
    if (!errors.isEmpty()) {
        text += QLatin1Char(' ') + errors.first();
    }
    return text;
}
PackageImportResult importDataPackage(const DataPackage &package,
                                      const QString &key,
                                      const QString &folder,
                                      UserObjects *objects,
                                      DataSources *data,
                                      MapSources *maps)
{
    PackageImportResult result;
    const auto title = package.name.isEmpty() ? tr("Data package") : package.name;
    const auto attribution = tr("Data package %1").arg(title).left(300);
    bool folderReady = false;
    const auto prepare = [&]() {
        if (!folderReady) {
            folderReady = QDir().mkpath(folder) &&
                          QFile::setPermissions(folder,
                                                QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                    QFileDevice::ExeOwner);
        }
        return folderReady;
    };
    // Write every file first, so the parts of a shapefile exist before its feed starts.
    for (const auto &entry : package.entries) {
        const auto suffix = QFileInfo(entry.path).suffix().toLower();
        if (cotFile(entry, suffix)) {
            continue;
        }
        if (!VectorSuffixes.contains(suffix) && !RasterSuffixes.contains(suffix) &&
            !ShapefileParts.contains(suffix)) {
            result.skipped.append(entry.path);
            continue;
        }
        if (!prepare()) {
            result.errors.append(tr("Cannot create the package folder."));
            return result;
        }
        if (const auto error = writeFile(folder, entry); !error.isEmpty()) {
            result.errors.append(error);
        }
    }
    int index = 0;
    for (const auto &entry : package.entries) {
        const auto suffix = QFileInfo(entry.path).suffix().toLower();
        const auto id = QStringLiteral("package-%1-%2").arg(key.left(16)).arg(index++);
        const auto name =
            QStringLiteral("%1: %2").arg(title, QFileInfo(entry.path).fileName()).left(100);
        const auto path = QFileInfo(QDir(folder).filePath(entry.path)).absoluteFilePath();
        if (cotFile(entry, suffix)) {
            const auto decoded = decodeCot(entry.bytes);
            const auto item = decoded.event ? localObjectFromCot(*decoded.event) : std::nullopt;
            if (!item || !objects) {
                result.skipped.append(entry.path);
                continue;
            }
            if (const auto error = objects->put(*item); !error.isEmpty()) {
                result.errors.append(QStringLiteral("%1: %2").arg(entry.path, error));
                continue;
            }
            ++result.objects;
        } else if (VectorSuffixes.contains(suffix)) {
            if (!data) {
                result.skipped.append(entry.path);
                continue;
            }
            auto source = dataDefaults();
            source.insert(QStringLiteral("name"), name);
            source.insert(QStringLiteral("endpoint"), path);
            source.insert(QStringLiteral("attribution"), attribution);
            source.insert(QStringLiteral("format"), QStringLiteral("org.swak.gdal.vector"));
            source.insert(QStringLiteral("enabled"), true);
            if (const auto error = data->put(id, source); !error.isEmpty()) {
                result.errors.append(QStringLiteral("%1: %2").arg(entry.path, error));
                continue;
            }
            result.feeds.append(id);
        } else if (RasterSuffixes.contains(suffix)) {
            if (!maps) {
                result.skipped.append(entry.path);
                continue;
            }
            auto source = mapDefaults(name, path, attribution, 19);
            source.insert(QStringLiteral("format"), QStringLiteral("org.swak.gdal.raster"));
            source.insert(QStringLiteral("service"), QStringLiteral("dataset"));
            if (const auto error = maps->put(id, source); !error.isEmpty()) {
                result.errors.append(QStringLiteral("%1: %2").arg(entry.path, error));
                continue;
            }
            result.maps.append(id);
        }
    }
    return result;
}
} // namespace swak

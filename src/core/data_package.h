#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace swak {
// Limits for TAK data packages (mission packages). A package is a zip archive with an optional
// MANIFEST/manifest.xml that lists its contents.
inline constexpr qsizetype MaxPackageBytes = 64 * 1024 * 1024;
inline constexpr int MaxPackageEntries = 256;
inline constexpr qsizetype MaxPackageEntryBytes = 64 * 1024 * 1024;
inline constexpr qsizetype MaxPackageExpansion = 128 * 1024 * 1024;

struct PackageEntry
{
    // A relative path with forward slashes and no dot segments.
    QString path;
    QByteArray bytes;
};
struct DataPackage
{
    QString uid;
    QString name;
    // The entries that the manifest lists, or all entries without a manifest. The manifest
    // itself and entries that the manifest marks with ignore="true" are not in the list.
    QList<PackageEntry> entries;
    QString error;
};
// True for a relative archive path that cannot leave its folder.
bool safePackagePath(const QString &path);
// Reads a zip data package from memory. It rejects links, special files, encrypted entries,
// unsafe and duplicate paths, and content beyond the limits.
DataPackage readDataPackage(const QByteArray &zip);
// Writes a zip data package with a version 2 manifest. Returns an empty array and an error
// for unsafe paths or content beyond the limits.
QByteArray writeDataPackage(const QString &uid,
                            const QString &name,
                            const QList<PackageEntry> &entries,
                            QString *error);
} // namespace swak

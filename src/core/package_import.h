#pragma once
#include "core/data_package.h"

#include <QStringList>

namespace swak {
class DataSources;
class MapSources;
class UserObjects;

struct PackageImportResult
{
    int objects = 0;
    QStringList feeds;
    QStringList maps;
    // Package paths that SWAK cannot use, such as images and unknown CoT types.
    QStringList skipped;
    QStringList errors;
    // A one-line summary for status lines.
    QString summary() const;
};
// Imports a data package.
//
// CoT files become local objects. Vector files become data feeds and raster files become map
// providers. SWAK writes the files under folder with owner-only permissions, so sibling files,
// such as the parts of a shapefile, stay together. A new import of the same package replaces
// its objects and feeds.
PackageImportResult importDataPackage(const DataPackage &package,
                                      const QString &key,
                                      const QString &folder,
                                      UserObjects *objects,
                                      DataSources *data,
                                      MapSources *maps);
} // namespace swak

#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

class GDALDataset;
namespace swak {
// Starts GDAL once with the project limits. Call it before any other GDAL function.
// The call is thread-safe. GDAL runs in the application process; see docs/dependencies.md.
void initializeGdal();
QString gdalVersion();
// Adds work for shutdownGdal, such as closing cached datasets.
void addGdalShutdownTask(std::function<void()> task);
// Closes cached GDAL resources on the calling thread. Call it once from the main thread after
// all GDAL workers stop, before the process exits. Shutdown tasks run in reverse order.
void shutdownGdal();
// Calls shutdownGdal when it leaves scope. Declare it before the objects that use GDAL.
struct GdalShutdownGuard final
{
    GdalShutdownGuard() = default;
    ~GdalShutdownGuard()
    {
        shutdownGdal();
    }
    GdalShutdownGuard(const GdalShutdownGuard &) = delete;
    GdalShutdownGuard &operator=(const GdalShutdownGuard &) = delete;
};
// Drivers that may read downloaded or user-selected files. The list excludes drivers that
// open other files, run programs, or contact servers on their own.
const std::vector<const char *> &gdalVectorDrivers();
// Raster file drivers. The remote list also excludes VRT, which can read local files.
const std::vector<const char *> &gdalRasterDrivers();
const std::vector<const char *> &gdalRemoteRasterDrivers();
// Replaces URL query values in diagnostic text. Query values can hold API keys.
QString redactQueryValues(const QString &text);
// True when the text declares a DTD. Some GDAL XML drivers expand internal entities.
bool declaresXmlDocumentType(const QByteArray &bytes);
// Reads the last GDAL error of this thread as bounded plain text.
QString gdalError(const QString &fallback);

struct GdalDatasetClose
{
    void operator()(GDALDataset *dataset) const noexcept;
};
using GdalDataset = std::unique_ptr<GDALDataset, GdalDatasetClose>;

// Owns a copy of the bytes as one file in the GDAL memory file system.
class GdalMemoryFile final
{
public:
    GdalMemoryFile(const QByteArray &bytes, const QString &suffix);
    ~GdalMemoryFile();
    GdalMemoryFile(const GdalMemoryFile &) = delete;
    GdalMemoryFile &operator=(const GdalMemoryFile &) = delete;
    const QByteArray &path() const
    {
        return path_;
    }
    bool isValid() const
    {
        return valid_;
    }

private:
    QByteArray path_;
    bool valid_ = false;
};
} // namespace swak

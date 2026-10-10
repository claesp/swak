#include "core/files.h"

#include "core/event.h"

#include <QCoreApplication>
#include <QFile>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace swak {
namespace {
class FileDescriptor final
{
public:
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor()
    {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    int get() const
    {
        return value_;
    }

private:
    int value_;
};
} // namespace
FileResult readRegularFile(const QString &path, qsizetype limit)
{
    const auto tr = [](const char *text) {
        return QCoreApplication::translate("Files", text);
    };
    if (limit <= 0 || limit > MaxRegularFileBytes) {
        return {{}, tr("The file size limit is invalid.")};
    }
    if (path.contains(QChar::Null)) {
        return {{}, tr("The file path contains a null character.")};
    }
    const FileDescriptor fd(
        ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (fd.get() < 0) {
        return {{}, tr("The file cannot be opened.")};
    }
    struct stat information{};
    if (::fstat(fd.get(), &information) != 0 || !S_ISREG(information.st_mode)) {
        return {{}, tr("Select a regular file.")};
    }
    if (information.st_size > limit) {
        return {{}, tr("The file exceeds the supported size limit.")};
    }
    QFile file;
    if (!file.open(fd.get(), QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) {
        return {{}, tr("The file cannot be read.")};
    }
    const auto bytes = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > limit) {
        return {{}, tr("The file cannot be read within the size limit.")};
    }
    return {bytes, {}};
}
FileResult readEventFile(const QString &path)
{
    return readRegularFile(path, MaxMessageBytes);
}
} // namespace swak

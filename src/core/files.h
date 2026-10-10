#pragma once
#include <QByteArray>
#include <QString>

namespace swak {
// The largest limit that readRegularFile accepts. The largest caller reads report attachments.
inline constexpr qsizetype MaxRegularFileBytes = 16 * 1024 * 1024;
struct FileResult
{
    QByteArray bytes;
    QString error;
};
FileResult readRegularFile(const QString &path, qsizetype limit);
FileResult readEventFile(const QString &path);
} // namespace swak

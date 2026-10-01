#pragma once
#include <QByteArray>
#include <QString>

namespace swak {
struct FileResult {
    QByteArray bytes;
    QString error;
};
FileResult readRegularFile(const QString &path, qsizetype limit);
FileResult readEventFile(const QString &path);
}

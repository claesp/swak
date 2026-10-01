#pragma once
#include <QByteArray>
#include <QString>

namespace swak {
struct FileResult {
    QByteArray bytes;
    QString error;
};
FileResult readEventFile(const QString &path);
}

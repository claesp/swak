#include "core/preferences.h"
#include "core/files.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace swak {
namespace {
constexpr qsizetype MaxPreferencesBytes = 1024 * 1024;
QString tr(const char *text) { return QCoreApplication::translate("Preferences", text); }
}
Preferences::Preferences(QString path) : path_(QFileInfo(path).absoluteFilePath())
{
    if (path.isEmpty() || path.contains(QChar::Null)) { error_ = tr("The preferences path is invalid."); return; }
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
        error_ = tr("The preferences directory cannot be created.");
        return;
    }
    lock_ = std::make_unique<QLockFile>(path_ + QStringLiteral(".lock"));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) {
        error_ = tr("Another instance owns the preferences file. This instance will not save preferences.");
        return;
    }
    if (QFileInfo::exists(path_)) {
        const auto input = readRegularFile(path_, MaxPreferencesBytes);
        if (!input.error.isEmpty()) { error_ = input.error; return; }
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(input.bytes, &error);
        if (error.error != QJsonParseError::NoError || !json.isObject() ||
            !json.object().value(QStringLiteral("version")).isDouble()) {
            error_ = tr("The preferences file is invalid. It will not be overwritten.");
            return;
        }
        if (json.object().value(QStringLiteral("version")).toDouble() != 1) {
            error_ = tr("The preferences version is unsupported. It will not be overwritten.");
            return;
        }
        document_ = json.object();
    }
    writable_ = true;
}
Preferences::~Preferences() = default;
QString Preferences::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/preferences.json");
}
bool Preferences::save(const QJsonObject &document)
{
    if (!writable_) return false;
    if (document == document_ && error_.isEmpty()) return true;
    QJsonObject output = document;
    output.insert(QStringLiteral("version"), 1);
    const auto bytes = QJsonDocument(output).toJson();
    if (bytes.size() > MaxPreferencesBytes) { error_ = tr("The preferences exceed the storage limit."); return false; }
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        error_ = tr("Preferences cannot be saved: %1").arg(file.errorString());
        return false;
    }
    document_ = output;
    error_.clear();
    return true;
}
double preferenceNumber(const QJsonObject &values, const QString &key, double fallback, double low, double high)
{
    const auto value = values.value(key);
    if (!value.isDouble()) return fallback;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= low && number <= high ? number : fallback;
}
int preferenceInteger(const QJsonObject &values, const QString &key, int fallback, int low, int high)
{
    const double number = preferenceNumber(values, key, fallback, low, high);
    return std::floor(number) == number ? static_cast<int>(number) : fallback;
}
QString preferenceText(const QJsonObject &values, const QString &key, const QString &fallback, qsizetype limit)
{
    const auto value = values.value(key);
    if (!value.isString()) return fallback;
    const auto text = value.toString();
    return text.size() <= limit && !text.contains(QChar::Null) ? text : fallback;
}
QRect fitWindowRectangle(QRect rectangle, const QList<QRect> &screens)
{
    if (screens.isEmpty()) return rectangle;
    QRect screen = screens.first();
    qint64 largest = -1;
    for (const auto &candidate : screens) {
        const auto overlap = rectangle.intersected(candidate);
        const auto area = static_cast<qint64>(overlap.width()) * overlap.height();
        if (area > largest) { largest = area; screen = candidate; }
    }
    rectangle.setSize(rectangle.size().expandedTo(QSize(1, 1)).boundedTo(screen.size()));
    rectangle.moveLeft(std::clamp(rectangle.x(), screen.left(), screen.right() - rectangle.width() + 1));
    rectangle.moveTop(std::clamp(rectangle.y(), screen.top(), screen.bottom() - rectangle.height() + 1));
    return rectangle;
}
}

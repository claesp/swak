#pragma once

#include <QJsonObject>
#include <QList>
#include <QRect>
#include <QString>

#include <memory>

class QLockFile;
namespace swak {
class PreferenceParticipant
{
public:
    virtual ~PreferenceParticipant() = default;
    virtual QString preferenceId() const = 0;
    virtual QString preferenceFallbackId() const
    {
        return {};
    }
    virtual QJsonObject savePreferences() const = 0;
    virtual void restorePreferences(const QJsonObject &values) = 0;
};

class Preferences final
{
public:
    explicit Preferences(QString path);
    ~Preferences();
    Preferences(const Preferences &) = delete;
    Preferences &operator=(const Preferences &) = delete;
    static QString defaultPath();
    const QJsonObject &document() const
    {
        return document_;
    }
    const QString &error() const
    {
        return error_;
    }
    const QString &path() const
    {
        return path_;
    }
    bool save(const QJsonObject &document);
    bool writable() const
    {
        return writable_;
    }

private:
    QString path_;
    QString error_;
    QJsonObject document_;
    std::unique_ptr<QLockFile> lock_;
    bool writable_ = false;
};

double preferenceNumber(
    const QJsonObject &values, const QString &key, double fallback, double low, double high);
int preferenceInteger(
    const QJsonObject &values, const QString &key, int fallback, int low, int high);
QString preferenceText(const QJsonObject &values,
                       const QString &key,
                       const QString &fallback,
                       qsizetype limit);
// Saved widget state, such as a splitter or a table header, as base64 text.
QString widgetStateText(const QByteArray &state);
// Reads widget state that widgetStateText wrote, as at most limit characters of base64 text. Missing
// or invalid text gives an empty array, which restoreState ignores.
QByteArray widgetStateBytes(const QJsonObject &values, const QString &key, qsizetype limit);
QRect fitWindowRectangle(QRect rectangle, const QList<QRect> &screens);
} // namespace swak

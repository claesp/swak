#pragma once

#include <QJsonObject>
#include <QList>
#include <QRect>
#include <QString>
#include <memory>

class QLockFile;
namespace swak {
class PreferenceParticipant {
public:
    virtual ~PreferenceParticipant() = default;
    virtual QString preferenceId() const = 0;
    virtual QJsonObject savePreferences() const = 0;
    virtual void restorePreferences(const QJsonObject &values) = 0;
};

class Preferences final {
public:
    explicit Preferences(QString path);
    ~Preferences();
    Preferences(const Preferences &) = delete;
    Preferences &operator=(const Preferences &) = delete;
    static QString defaultPath();
    const QJsonObject &document() const { return document_; }
    const QString &error() const { return error_; }
    const QString &path() const { return path_; }
    bool save(const QJsonObject &document);
private:
    QString path_;
    QString error_;
    QJsonObject document_;
    std::unique_ptr<QLockFile> lock_;
    bool writable_ = false;
};

double preferenceNumber(const QJsonObject &values, const QString &key, double fallback, double low, double high);
int preferenceInteger(const QJsonObject &values, const QString &key, int fallback, int low, int high);
QString preferenceText(const QJsonObject &values, const QString &key, const QString &fallback, qsizetype limit);
QRect fitWindowRectangle(QRect rectangle, const QList<QRect> &screens);
}

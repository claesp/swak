#pragma once
#include "core/settings.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointF>
#include <QPointer>
#include <QTimer>

namespace swak {
struct RouteResult
{
    QList<QPointF> coordinates;
    double meters = 0;
    double seconds = 0;
    QString error;
};
class RouteProvider
{
public:
    virtual ~RouteProvider() = default;
    virtual QString routeId() const = 0;
    virtual QString routeName() const = 0;
    virtual QJsonObject presets() const
    {
        return {};
    }
    virtual QJsonObject
    request(QPointF start, QPointF end, const QString &mode, double speed) const = 0;
    virtual RouteResult decode(const QByteArray &body) const = 0;
};
class RouteSources final : public QObject, public PreferenceParticipant
{
    Q_OBJECT
public:
    RouteSources(const QList<QObject *> &plugins, QObject *parent);
    ~RouteSources() override
    {
        cancel();
    }
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.routes");
    }
    QJsonObject savePreferences() const override
    {
        return values_;
    }
    void restorePreferences(const QJsonObject &values) override;
    const QList<RouteProvider *> &providers() const
    {
        return providers_;
    }
    RouteProvider *provider(const QJsonObject &source) const;
    QJsonObject sources() const
    {
        return values_.value(QStringLiteral("sources")).toObject();
    }
    QString activeId() const
    {
        return values_.value(QStringLiteral("active")).toString();
    }
    QString put(const QString &id, const QJsonObject &source);
    void select(const QString &id);
    void remove(const QString &id);
    void calculate(QPointF start, QPointF end, const QString &mode, double speed);
    void cancel();
    bool busy() const
    {
        return !reply_.isNull();
    }
    const RouteResult &result() const
    {
        return result_;
    }
    static QString validate(const QJsonObject &source);
    static QList<SettingField> fields();
signals:
    void changed();
    void completed();

private:
    QList<RouteProvider *> providers_;
    QJsonObject values_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QTimer deadline_;
    QTimer cooldown_;
    QByteArray body_;
    RouteResult result_;
    void fail(const QString &message);
};
} // namespace swak
Q_DECLARE_INTERFACE(swak::RouteProvider, "org.swak.RouteProvider/1.0")

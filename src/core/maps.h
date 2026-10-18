#pragma once
#include "core/settings.h"

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QNetworkRequest>
#include <QObject>
#include <QSet>
#include <QUrl>

#include <atomic>

namespace swak {
class MapExtension
{
public:
    enum class Kind
    {
        Provider,
        Tool
    };
    virtual ~MapExtension() = default;
    virtual QString mapId() const = 0;
    virtual QString mapName() const = 0;
    virtual Kind mapKind() const = 0;
};
class MapProvider : public MapExtension
{
public:
    Kind mapKind() const override
    {
        return Kind::Provider;
    }
    virtual QJsonObject defaults() const = 0;
    virtual QJsonObject additionalSources() const
    {
        return {};
    }
    virtual QList<SettingField> fields() const = 0;
    virtual QString validate(const QJsonObject &source) const = 0;
    virtual QUrl tileUrl(const QJsonObject &source, int zoom, int x, int y) const = 0;
    // Earlier format identifiers. The catalog converts their entries with migrate().
    virtual QStringList legacyIds() const
    {
        return {};
    }
    virtual QJsonObject migrate(const QJsonObject &source) const
    {
        return source;
    }
};
struct RasterRequest
{
    QJsonObject source;
    QByteArray credential;
    // An empty directory disables the provider disk cache, for example for credentials.
    QString cacheDirectory;
    qint64 cacheBytes = 0;
    int zoom = 0;
    int x = 0;
    int y = 0;
};
struct RasterTile
{
    QImage image;
    QString error;
    bool providerWide = false;
};
// A provider that renders raster tiles itself instead of returning tile URLs.
class RasterTileProvider
{
public:
    virtual ~RasterTileProvider() = default;
    // The host calls this method on a worker thread. Keep it reentrant.
    virtual RasterTile renderTile(const RasterRequest &request,
                                  const std::atomic_bool &cancelled) const = 0;
};
// A session that an online map service issued for later tile and attribution requests.
struct MapSession
{
    QString token;
    QDateTime expires;
};
// Geographic bounds of a view in degrees. West and east stay within -180 and 180.
struct MapBounds
{
    double west = -180;
    double south = -85;
    double east = 180;
    double north = 85;
};
// Optional services of an online map provider. The host calls these methods on the GUI thread.
// The host sends every request to the source endpoint host, with the source credentials.
class MapServiceProvider
{
public:
    virtual ~MapServiceProvider() = default;
    // A POST URL that starts a session. An empty URL means that the service has no sessions.
    virtual QUrl sessionUrl(const QJsonObject &) const
    {
        return {};
    }
    // The JSON body of the session request.
    virtual QByteArray sessionBody(const QJsonObject &) const
    {
        return {};
    }
    // Reads a session reply. An empty token means that the reply is not usable.
    virtual MapSession readSession(const QByteArray &) const
    {
        return {};
    }
    // The tile URL for a session. MapProvider::tileUrl serves services without sessions.
    virtual QUrl sessionTileUrl(const QJsonObject &, const QString &, int, int, int) const
    {
        return {};
    }
    // A GET URL for the attribution of a view. The session is empty for services without one.
    virtual QUrl attributionUrl(const QJsonObject &source,
                                const QString &session,
                                int zoom,
                                const MapBounds &bounds) const = 0;
    // Reads an attribution reply as text or simple HTML. The host converts it to plain text.
    virtual QString readAttribution(const QByteArray &reply) const = 0;
};
QList<SettingField> commonMapFields();
QString validateMapSource(const QJsonObject &source);
// Checks the name, attribution, location text, zoom, and authentication fields.
// It does not require an HTTPS location.
QString validateMapText(const QJsonObject &source);
QJsonObject mapDefaults(const QString &name,
                        const QString &endpoint,
                        const QString &attribution,
                        int maximumZoom);
QString validateMapAuthentication(const QJsonObject &source);
// Checks a source of a fixed online service: its endpoint, an API key query parameter,
// and certificate validation. Users cannot point these sources at another server.
QString validateServiceSource(const QJsonObject &source,
                              const QString &endpoint,
                              const QString &keyParameter);
// Checks an optional IETF language tag, such as en-US.
bool validLanguageTag(const QString &tag);
QString
authorizeMapRequest(QNetworkRequest &request, const QJsonObject &source, const QByteArray &secret);
bool authenticatedMap(const QJsonObject &source);
bool validTile(int zoom, int x, int y);

class MapSources final : public QObject, public SettingsParticipant
{
    Q_OBJECT
public:
    explicit MapSources(const QList<QObject *> &plugins, QObject *parent = nullptr);
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.maps");
    }
    QString settingsTitle() const override
    {
        return tr("Map providers");
    }
    QList<SettingField> settingsFields() const override;
    QString validateSettings(const QJsonObject &values) const override;
    QJsonObject savePreferences() const override
    {
        return values_;
    }
    void restorePreferences(const QJsonObject &values) override;
    const QList<MapProvider *> &providers() const
    {
        return providers_;
    }
    MapProvider *provider(const QJsonObject &source) const;
    QJsonObject sources() const
    {
        return values_.value(QStringLiteral("sources")).toObject();
    }
    QJsonObject activeSource() const;
    QString activeId() const
    {
        return values_.value(QStringLiteral("active")).toString();
    }
    QString put(const QString &id, const QJsonObject &source);
    void remove(const QString &id);
    QString setCredential(const QString &id, const QByteArray &secret);
    QByteArray credential(const QString &id) const;
    void select(const QString &id);
    bool networkEnabled() const
    {
        return values_.value(QStringLiteral("network")).toBool(false);
    }
signals:
    void managementRequested();
    void changed();

private:
    void addNewPresets();
    QList<MapProvider *> providers_;
    QJsonObject values_;
    QHash<QString, QByteArray> credentials_;
    // Invalid providers that the log already reported.
    QSet<QString> reportedInvalid_;
};

} // namespace swak
Q_DECLARE_INTERFACE(swak::MapExtension, "org.swak.MapExtension/1.0")
Q_DECLARE_INTERFACE(swak::MapProvider, "org.swak.MapProvider/1.0")
Q_DECLARE_INTERFACE(swak::RasterTileProvider, "org.swak.RasterTiles/1.0")
Q_DECLARE_INTERFACE(swak::MapServiceProvider, "org.swak.MapService/1.0")

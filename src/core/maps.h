#pragma once
#include "core/settings.h"
#include <QObject>
#include <QUrl>

namespace swak {
class MapExtension {
public:
    enum class Kind { Provider, Tool };
    virtual ~MapExtension() = default;
    virtual QString mapId() const = 0;
    virtual QString mapName() const = 0;
    virtual Kind mapKind() const = 0;
};
class MapProvider : public MapExtension {
public:
    Kind mapKind() const override { return Kind::Provider; }
    virtual QJsonObject defaults() const = 0;
    virtual QJsonObject additionalSources() const { return {}; }
    virtual QList<SettingField> fields() const = 0;
    virtual QString validate(const QJsonObject &source) const = 0;
    virtual QUrl tileUrl(const QJsonObject &source, int zoom, int x, int y) const = 0;
};
QList<SettingField> commonMapFields();
QString validateMapSource(const QJsonObject &source);
QJsonObject mapDefaults(const QString &name, const QString &endpoint, const QString &attribution, int maximumZoom);
bool validTile(int zoom, int x, int y);

class MapSources final : public QObject, public SettingsParticipant {
    Q_OBJECT
public:
    explicit MapSources(const QList<QObject *> &plugins, QObject *parent = nullptr);
    QString preferenceId() const override { return QStringLiteral("org.swak.maps"); }
    QString settingsTitle() const override { return tr("Map providers"); }
    QList<SettingField> settingsFields() const override;
    QString validateSettings(const QJsonObject &values) const override;
    QJsonObject savePreferences() const override { return values_; }
    void restorePreferences(const QJsonObject &values) override;
    const QList<MapProvider *> &providers() const { return providers_; }
    MapProvider *provider(const QJsonObject &source) const;
    QJsonObject sources() const { return values_.value(QStringLiteral("sources")).toObject(); }
    QJsonObject activeSource() const;
    QString activeId() const { return values_.value(QStringLiteral("active")).toString(); }
    QString put(const QString &id, const QJsonObject &source);
    void remove(const QString &id);
    void select(const QString &id);
    bool networkEnabled() const { return values_.value(QStringLiteral("network")).toBool(false); }
signals:
    void managementRequested();
    void changed();
private:
    void addNewPresets();
    QList<MapProvider *> providers_;
    QJsonObject values_;
};

}
Q_DECLARE_INTERFACE(swak::MapExtension, "org.swak.MapExtension/1.0")
Q_DECLARE_INTERFACE(swak::MapProvider, "org.swak.MapProvider/1.0")

#pragma once
#include "core/settings.h"

#include <QColor>
#include <QObject>
#include <QPainterPath>

#include <optional>
class QPainter;
namespace swak {
inline const QString CoreMarkerCollection = QStringLiteral("org.swak.core-markers");
inline const QString MilitaryMarkerCollection = QStringLiteral("org.swak.milstd2525");
class MarkerCollectionProvider
{
public:
    virtual ~MarkerCollectionProvider() = default;
    virtual QJsonObject markerCollection() const = 0;
};
struct CollectionSymbol
{
    QString id;
    QString name;
    QColor color;
    QPainterPath frame;
    QPainterPath glyph;
};
QJsonArray encodeMarkerPath(const QPainterPath &path);
QJsonObject coreMarkerCollection();
QJsonObject militaryMarkerCollection();
QString effectiveMarkerCollection(const QString &collection, const QString &symbol);
void paintCollectionSymbol(QPainter &painter,
                           const CollectionSymbol &symbol,
                           QPointF center,
                           double radius,
                           const QColor &color);
class MarkerCollections final : public QObject, public SettingsParticipant
{
    Q_OBJECT
public:
    explicit MarkerCollections(const QList<QObject *> &plugins, QObject *parent = nullptr);
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.markercollections");
    }
    QString settingsTitle() const override
    {
        return tr("Marker collections");
    }
    QList<SettingField> settingsFields() const override
    {
        return {};
    }
    QJsonObject savePreferences() const override;
    void restorePreferences(const QJsonObject &values) override;
    const QJsonObject &collections() const
    {
        return entries_;
    }
    bool builtIn(const QString &id) const
    {
        return builtins_.contains(id);
    }
    QString put(const QJsonObject &collection);
    QString remove(const QString &id);
    QString setEnabled(const QString &id, bool enabled);
    std::optional<CollectionSymbol> symbol(const QString &collection, const QString &id) const;
    static QString validate(const QJsonObject &collection);
    QString importArchive(const QString &path);
    QString exportArchive(const QString &id, const QString &path) const;
    const QString &directory() const
    {
        return directory_;
    }
    void setDirectory(const QString &value)
    {
        directory_ = value.left(4096);
    }
signals:
    void changed();

private:
    void rebuild();
    QJsonObject entries_;
    QJsonObject builtins_;
    QHash<QString, CollectionSymbol> symbols_;
    QString directory_;
};
} // namespace swak
Q_DECLARE_INTERFACE(swak::MarkerCollectionProvider, "org.swak.MarkerCollection/1.0")

#pragma once
#include "core/map_selection.h"
#include "core/projection.h"
#include "core/settings.h"

#include <QPainterPath>
#include <QThreadPool>

#include <atomic>
#include <memory>

class QPainter;
namespace swak {
struct DataShape
{
    enum class Kind
    {
        Point,
        Line,
        Polygon
    };
    Kind kind = Kind::Point;
    QPainterPath path;
    QPointF point;
    QString name;
    QString description;
    QJsonObject properties;
};
struct DataDocument
{
    QList<DataShape> shapes;
    QString error;
    int ignoredLinks = 0;
    int ignoredOverlays = 0;
};
class DataProvider
{
public:
    virtual ~DataProvider() = default;
    virtual QString dataId() const = 0;
    virtual QString dataName() const = 0;
    virtual QList<QJsonObject> presets() const = 0;
    virtual DataDocument decode(const QByteArray &bytes,
                                const std::atomic_bool &cancelled) const = 0;
    // Reads a local file in place. Some formats need sibling files, such as a shapefile.
    virtual DataDocument decodeFile(const QString &path, const std::atomic_bool &cancelled) const
    {
        Q_UNUSED(path)
        Q_UNUSED(cancelled)
        DataDocument result;
        result.error = QObject::tr("This data format cannot read local files.");
        return result;
    }
    // Earlier format identifiers that saved profiles can still contain.
    virtual QStringList legacyIds() const
    {
        return {};
    }
};
// True when the endpoint names a local file instead of an HTTPS resource.
bool localDataEndpoint(const QString &endpoint);
QString localDataPath(const QString &endpoint);
inline constexpr qsizetype DataDownloadLimit = 8 * 1024 * 1024;
inline constexpr qsizetype DataXmlLimit = 16 * 1024 * 1024;
QList<SettingField> dataFields();
QJsonObject dataDefaults();
QString validateDataSource(const QJsonObject &source);
class DataSources final : public QObject, public SettingsParticipant
{
    Q_OBJECT
public:
    explicit DataSources(const QList<QObject *> &plugins, QObject *parent = nullptr);
    ~DataSources() override;
    QString preferenceId() const override
    {
        return QStringLiteral("org.swak.data");
    }
    QString settingsTitle() const override
    {
        return tr("Data providers");
    }
    QList<SettingField> settingsFields() const override;
    QString validateSettings(const QJsonObject &values) const override;
    QJsonObject savePreferences() const override
    {
        return values_;
    }
    void restorePreferences(const QJsonObject &values) override;
    QJsonObject sources() const
    {
        return values_.value(QStringLiteral("sources")).toObject();
    }
    const QList<DataProvider *> &providers() const
    {
        return providers_;
    }
    DataProvider *provider(const QJsonObject &source) const;
    QString put(const QString &id, const QJsonObject &source);
    void remove(const QString &id);
    void refresh(const QString &id);
    QString status(const QString &id) const;
    int shapeCount(const QString &id) const;
    QStringList attributions() const;
    void paint(QPainter &painter, const Viewport &view, const MapItem *selected = nullptr) const;
    QList<MapItem> itemsAt(const Viewport &view, QPointF position) const;
signals:
    void changed();
    void contentChanged();

private:
    struct Feed;
    void reconcile();
    void pump();
    void fetch(const QString &id, const std::shared_ptr<Feed> &feed);
    void decodeLocal(const QString &id, const std::shared_ptr<Feed> &feed, const QString &path);
    void finish(const QString &id, const std::shared_ptr<Feed> &feed, DataDocument document);
    void retire(const std::shared_ptr<Feed> &feed);
    QList<DataProvider *> providers_;
    QJsonObject values_;
    QHash<QString, std::shared_ptr<Feed>> feeds_;
    QThreadPool pool_;
    bool stopping_ = false;
    int activeJobs_ = 0;
};
} // namespace swak
Q_DECLARE_INTERFACE(swak::DataProvider, "org.swak.DataProvider/1.0")

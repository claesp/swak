#include "core/data.h"

#include "core/logging.h"
#include "core/map_label.h"
#include "core/text.h"

#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <utility>

namespace swak {
bool localDataEndpoint(const QString &endpoint)
{
    return endpoint.startsWith('/') || endpoint.startsWith(QStringLiteral("file:"));
}
QString localDataPath(const QString &endpoint)
{
    if (endpoint.startsWith('/')) {
        return endpoint;
    }
    const QUrl url(endpoint, QUrl::StrictMode);
    return url.isValid() && url.isLocalFile() && url.host().isEmpty() ? url.toLocalFile()
                                                                      : QString{};
}
QJsonObject dataDefaults()
{
    return {{QStringLiteral("name"), QString{}},
            {QStringLiteral("endpoint"), QString{}},
            {QStringLiteral("attribution"), QString{}},
            {QStringLiteral("enabled"), false},
            {QStringLiteral("interval"), 300},
            {QStringLiteral("ignoreTls"), false},
            {QStringLiteral("color"), QStringLiteral("#ffca64")},
            {QStringLiteral("opacity"), 0.85},
            {QStringLiteral("lineWidth"), 2},
            {QStringLiteral("pointSize"), 5},
            {QStringLiteral("fillOpacity"), 0.18},
            {QStringLiteral("labels"), true}};
}
QList<SettingField> dataFields()
{
    return {
        textSetting(QStringLiteral("name"), QObject::tr("Name"), 100),
        textSetting(QStringLiteral("endpoint"), QObject::tr("HTTPS URL or local file path"), 2048),
        textSetting(QStringLiteral("attribution"), QObject::tr("Attribution and license URL"), 300),
        booleanSetting(QStringLiteral("enabled"), QObject::tr("Enabled")),
        numberSetting(
            QStringLiteral("interval"), QObject::tr("Refresh interval (seconds)"), 60, 86400),
        booleanSetting(QStringLiteral("ignoreTls"),
                       QObject::tr("Ignore invalid HTTPS certificates (unsafe)")),
        textSetting(QStringLiteral("color"), QObject::tr("Color (#RRGGBB)"), 7),
        numberSetting(QStringLiteral("opacity"), QObject::tr("Opacity"), 0, 1, 2),
        numberSetting(QStringLiteral("lineWidth"), QObject::tr("Line width"), 1, 10, 1),
        numberSetting(QStringLiteral("pointSize"), QObject::tr("Point radius"), 2, 20, 1),
        numberSetting(QStringLiteral("fillOpacity"), QObject::tr("Polygon fill opacity"), 0, 1, 2),
        booleanSetting(QStringLiteral("labels"), QObject::tr("Labels"))};
}
QString validateDataSource(const QJsonObject &source)
{
    if (auto error = validateSourceText(
            source, QObject::tr("Enter a name, location, and attribution within their limits."));
        !error.isEmpty()) {
        return error;
    }
    const auto endpoint = source.value(QStringLiteral("endpoint")).toString();
    if (localDataEndpoint(endpoint)) {
        const auto path = localDataPath(endpoint);
        if (path.isEmpty() || !QDir::isAbsolutePath(path) ||
            path.split('/').contains(QStringLiteral(".."))) {
            return QObject::tr("Use an absolute local file path.");
        }
    } else {
        const QUrl url(endpoint, QUrl::StrictMode);
        if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty() ||
            !url.userInfo().isEmpty() || url.hasFragment()) {
            return QObject::tr("Use an HTTPS URL without credentials or a fragment, or an "
                               "absolute local file path.");
        }
    }
    const auto fields = dataFields();
    for (const auto &field : fields) {
        if (field.kind == SettingField::Kind::Number) {
            const double value = source.value(field.path).toDouble(-1);
            if (!std::isfinite(value) || value < field.minimum || value > field.maximum ||
                (field.decimals == 0 && value != std::floor(value))) {
                return QObject::tr("A numeric data parameter is outside its range.");
            }
        }
    }
    static const QRegularExpression color(QStringLiteral("\\A#[0-9a-fA-F]{6}\\z"));
    if (!color.match(source.value(QStringLiteral("color")).toString()).hasMatch()) {
        return QObject::tr("Use a color in #RRGGBB form.");
    }
    return {};
}
struct DataSources::Feed
{
    QJsonObject source;
    // Drawing values from the source. The source of a feed never changes, so these are read
    // once. Painting and hit tests must not read the preference object for each shape.
    struct Style
    {
        QColor color;
        double opacity = 1;
        double lineWidth = 1;
        double pointSize = 1;
        double fillOpacity = 0;
        bool labels = false;
    } style;
    DataDocument document;
    QString status;
    QNetworkAccessManager *network = nullptr;
    QTimer *timer = nullptr;
    QPointer<QNetworkReply> reply;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    bool busy = false;
    bool stale = false;
    bool due = true;
};
DataSources::DataSources(const QList<QObject *> &plugins, QObject *parent) : QObject(parent)
{
    pool_.setMaxThreadCount(2);
    QJsonObject entries;
    for (auto *plugin : plugins) {
        if (auto *format = qobject_cast<DataProvider *>(plugin)) {
            providers_.append(format);
            int index = 0;
            for (const auto &source : format->presets()) {
                entries.insert(format->dataId() + QString::number(++index), source);
            }
        }
    }
    values_.insert(QStringLiteral("sources"), entries);
    reconcile();
}
DataSources::~DataSources()
{
    stopping_ = true;
    for (const auto &feed : feeds_) {
        retire(feed);
    }
    pool_.waitForDone();
}
void DataSources::retire(const std::shared_ptr<Feed> &feed)
{
    *feed->cancelled = true;
    if (feed->reply) {
        feed->reply->disconnect(this);
        feed->reply->abort();
        if (feed->busy) {
            feed->busy = false;
            --activeJobs_;
        }
    }
    delete feed->timer;
    feed->timer = nullptr;
    delete feed->network;
    feed->network = nullptr;
}
DataProvider *DataSources::provider(const QJsonObject &source) const
{
    const auto found =
        std::find_if(providers_.begin(), providers_.end(), [&](const auto *candidate) {
            const auto format = source.value(QStringLiteral("format")).toString();
            return candidate->dataId() == format || candidate->legacyIds().contains(format);
        });
    return found == providers_.end() ? nullptr : *found;
}
QList<SettingField> DataSources::settingsFields() const
{
    QList<SettingField> result;
    const auto entries = sources();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (provider(it.value().toObject())) {
            for (auto field : dataFields()) {
                field.path = QStringLiteral("sources/") + it.key() + '/' + field.path;
                field.label = it.value().toObject().value(QStringLiteral("name")).toString() +
                              QStringLiteral(": ") + field.label;
                result.append(field);
            }
        }
    }
    return result;
}
QString DataSources::validateSettings(const QJsonObject &values) const
{
    if (!values.value(QStringLiteral("sources")).isObject()) {
        return tr("Invalid data catalog.");
    }
    const auto entries = values.value(QStringLiteral("sources")).toObject();
    if (entries.size() > 8) {
        return tr("Use at most eight data feeds.");
    }
    static const QRegularExpression id(QStringLiteral("\\A[a-zA-Z0-9.-]{1,100}\\z"));
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!id.match(it.key()).hasMatch() || !it.value().isObject()) {
            return tr("Invalid data feed identifier.");
        }
        if (provider(it.value().toObject()) && sources().value(it.key()) != it.value()) {
            const auto error = validateDataSource(it.value().toObject());
            if (!error.isEmpty()) {
                return error;
            }
        }
    }
    return {};
}
void DataSources::restorePreferences(const QJsonObject &values)
{
    if (values.value(QStringLiteral("sources")).isObject() &&
        values.value(QStringLiteral("sources")).toObject().size() <= 8) {
        values_ = values;
        reconcile();
        emit changed();
    }
}
QString DataSources::put(const QString &id, const QJsonObject &source)
{
    auto next = values_;
    auto entries = sources();
    entries.insert(id, source);
    next.insert(QStringLiteral("sources"), entries);
    const auto error = validateSettings(next);
    if (!error.isEmpty()) {
        return error;
    }
    restorePreferences(next);
    return {};
}
void DataSources::remove(const QString &id)
{
    auto entries = sources();
    entries.remove(id);
    values_.insert(QStringLiteral("sources"), entries);
    reconcile();
    emit changed();
}
void DataSources::reconcile()
{
    const auto entries = sources();
    for (const auto &id : feeds_.keys()) {
        if (entries.value(id).toObject() != feeds_.value(id)->source) {
            retire(feeds_.take(id));
        }
    }
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (feeds_.contains(it.key())) {
            continue;
        }
        auto feed = std::make_shared<Feed>();
        feed->source = it.value().toObject();
        const auto &source = feed->source;
        feed->style = {QColor(source.value(QStringLiteral("color")).toString()),
                       source.value(QStringLiteral("opacity")).toDouble(),
                       source.value(QStringLiteral("lineWidth")).toDouble(),
                       source.value(QStringLiteral("pointSize")).toDouble(),
                       source.value(QStringLiteral("fillOpacity")).toDouble(),
                       source.value(QStringLiteral("labels")).toBool()};
        feeds_.insert(it.key(), feed);
        if (!provider(feed->source)) {
            feed->status = tr("Plugin unavailable. Configuration is preserved.");
            continue;
        }
        const auto error = validateDataSource(feed->source);
        if (!error.isEmpty()) {
            feed->status = error;
            continue;
        }
        if (!feed->source.value(QStringLiteral("enabled")).toBool()) {
            feed->status = tr("Disabled.");
            continue;
        }
        feed->network = new QNetworkAccessManager(this);
        feed->timer = new QTimer(this);
        feed->timer->setSingleShot(true);
        feed->timer->setObjectName(QStringLiteral("dataRefresh-") + it.key());
        feed->timer->setInterval(feed->source.value(QStringLiteral("interval")).toInt() * 1000);
        connect(feed->timer, &QTimer::timeout, this, [this, id = it.key()] {
            refresh(id);
        });
        feed->status = tr("Waiting for refresh.");
    }
    pump();
    emit contentChanged();
}
void DataSources::refresh(const QString &id)
{
    const auto feed = feeds_.value(id);
    if (!feed || !feed->network || feed->busy) {
        return;
    }
    feed->due = true;
    feed->timer->stop();
    pump();
}
void DataSources::pump()
{
    if (stopping_) {
        return;
    }
    auto ids = feeds_.keys();
    ids.sort();
    for (const auto &id : ids) {
        const auto feed = feeds_.value(id);
        if (activeJobs_ >= 2) {
            break;
        }
        if (feed->network && feed->due && !feed->busy) {
            fetch(id, feed);
        }
    }
}
void DataSources::fetch(const QString &id, const std::shared_ptr<Feed> &feed)
{
    ++activeJobs_;
    feed->busy = true;
    feed->due = false;
    feed->status = tr("Updating…");
    const auto endpoint = feed->source.value(QStringLiteral("endpoint")).toString();
    if (localDataEndpoint(endpoint)) {
        decodeLocal(id, feed, localDataPath(endpoint));
        emit contentChanged();
        return;
    }
    QNetworkRequest request(QUrl(feed->source.value(QStringLiteral("endpoint")).toString()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("User-Agent", "SWAK/0.1 data client");
    request.setRawHeader(
        "Accept",
        "application/vnd.google-earth.kml+xml, application/vnd.google-earth.kmz, application/xml");
    request.setTransferTimeout(20000);
    auto *reply = feed->network->get(request);
    feed->reply = reply;
    reply->setReadBufferSize(DataDownloadLimit + 1);
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    deadline->start(30000);
    if (feed->source.value(QStringLiteral("ignoreTls")).toBool()) {
        connect(reply, &QNetworkReply::sslErrors, reply, [reply] {
            reply->ignoreSslErrors();
        });
    }
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes] {
        bytes->append(reply->read(DataDownloadLimit + 1 - bytes->size()));
        if (bytes->size() > DataDownloadLimit) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, id, feed, reply, bytes] {
        if (reply->isOpen()) {
            bytes->append(reply->read(DataDownloadLimit + 1 - bytes->size()));
        }
        const bool ok = reply->error() == QNetworkReply::NoError &&
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
                        bytes->size() <= DataDownloadLimit;
        feed->reply = nullptr;
        reply->deleteLater();
        if (!ok) {
            DataDocument failed;
            failed.error = tr("Data download failed.");
            finish(id, feed, std::move(failed));
            return;
        }
        auto *watcher = new QFutureWatcher<DataDocument>(this);
        connect(watcher, &QFutureWatcher<DataDocument>::finished, this, [this, id, feed, watcher] {
            auto document = watcher->result();
            watcher->deleteLater();
            finish(id, feed, std::move(document));
        });
        const auto *format = provider(feed->source);
        watcher->setFuture(QtConcurrent::run(&pool_, [format, bytes, cancelled = feed->cancelled] {
            try {
                return format->decode(*bytes, *cancelled);
            } catch (const std::exception &) {
                DataDocument failed;
                failed.error = QObject::tr("Data decoder failed.");
                return failed;
            }
        }));
    });
    emit contentChanged();
}
void DataSources::decodeLocal(const QString &id,
                              const std::shared_ptr<Feed> &feed,
                              const QString &path)
{
    auto *watcher = new QFutureWatcher<DataDocument>(this);
    connect(watcher, &QFutureWatcher<DataDocument>::finished, this, [this, id, feed, watcher] {
        auto document = watcher->result();
        watcher->deleteLater();
        finish(id, feed, std::move(document));
    });
    const auto *format = provider(feed->source);
    watcher->setFuture(QtConcurrent::run(&pool_, [format, path, cancelled = feed->cancelled] {
        try {
            return format->decodeFile(path, *cancelled);
        } catch (const std::exception &) {
            DataDocument failed;
            failed.error = QObject::tr("Data decoder failed.");
            return failed;
        }
    }));
}
void DataSources::finish(const QString &id,
                         const std::shared_ptr<Feed> &feed,
                         DataDocument document)
{
    if (feed->busy) {
        feed->busy = false;
        --activeJobs_;
    }
    if (stopping_ || *feed->cancelled || feeds_.value(id) != feed) {
        pump();
        return;
    }
    if (document.error.isEmpty()) {
        feed->stale = false;
        feed->document = std::move(document);
        feed->status =
            tr("%1 shapes | Updated %2 UTC")
                .arg(feed->document.shapes.size())
                .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss")));
        if (feed->document.ignoredLinks || feed->document.ignoredOverlays) {
            feed->status += tr(" | Skipped %1 network links and %2 unsupported overlays or models.")
                                .arg(feed->document.ignoredLinks)
                                .arg(feed->document.ignoredOverlays);
        }
        logMessage(Severity::Info,
                   Urgency::Routine,
                   QStringLiteral("swak.data"),
                   feed->source.value(QStringLiteral("name")).toString() + QStringLiteral(": ") +
                       feed->status);
    } else {
        feed->stale = true;
        feed->status = document.error + tr(" Previous geometry is retained.");
        logMessage(Severity::Warning,
                   Urgency::Attention,
                   QStringLiteral("swak.data"),
                   feed->source.value(QStringLiteral("name")).toString() + QStringLiteral(": ") +
                       feed->status);
    }
    feed->timer->start();
    emit contentChanged();
    pump();
}
QString DataSources::status(const QString &id) const
{
    const auto feed = feeds_.value(id);
    return feed ? feed->status : QString{};
}
// Native tests inspect complete document replacement through this count.
// cppcheck-suppress unusedFunction
int DataSources::shapeCount(const QString &id) const
{
    const auto feed = feeds_.value(id);
    return feed ? static_cast<int>(feed->document.shapes.size()) : 0;
}
QStringList DataSources::attributions() const
{
    QStringList credits;
    auto ids = feeds_.keys();
    ids.sort();
    for (const auto &id : ids) {
        const auto feed = feeds_.value(id);
        if (feed->document.shapes.isEmpty()) {
            continue;
        }
        QString credit = (feed->stale ? tr("STALE | ") : QString{}) +
                         feed->source.value(QStringLiteral("attribution")).toString();
        if (feed->source.value(QStringLiteral("ignoreTls")).toBool()) {
            credit += tr(" | HTTPS certificate checks disabled");
        }
        credits.append(credit);
    }
    return credits;
}
void DataSources::paint(QPainter &painter, const Viewport &view, const MapItem *selected) const
{
    auto ids = feeds_.keys();
    ids.sort();
    const double worldSize = view.scale();
    const QPointF worldOrigin = view.toScreen({0, 0});
    painter.save();
    painter.setClipRect(QRectF(worldOrigin, QSizeF(worldSize, worldSize)), Qt::IntersectClip);
    int labels = 0;
    for (const auto &id : ids) {
        const auto feed = feeds_.value(id);
        if (feed->document.shapes.isEmpty()) {
            continue;
        }
        const auto &style = feed->style;
        const auto &color = style.color;
        painter.save();
        painter.setOpacity(style.opacity);
        QPen pen(color, style.lineWidth);
        painter.setPen(pen);
        const QPointF origin(view.size.width() / 2, view.size.height() / 2);
        const double scale = view.scale();
        const QRectF screen(QPointF(0, 0), view.size);
        for (const auto &shape : feed->document.shapes) {
            for (int wrap = -1; wrap <= 1; ++wrap) {
                const QPointF offset = origin - view.center * scale + QPointF(wrap * scale, 0);
                const QPointF point = shape.point * scale + offset;
                if (shape.kind == DataShape::Kind::Point) {
                    const double radius = view.markerSize(style.pointSize);
                    if (!screen.adjusted(-radius - 2, -radius - 2, radius + 2, radius + 2)
                             .contains(point)) {
                        continue;
                    }
                    painter.setBrush(color);
                    painter.drawEllipse(point, radius, radius);
                } else {
                    const auto bounds = shape.path.boundingRect();
                    if (!screen.intersects(
                            QRectF(bounds.topLeft() * scale + offset, bounds.size() * scale)
                                .adjusted(-10, -10, 10, 10))) {
                        continue;
                    }
                    painter.save();
                    QTransform mapping;
                    mapping.translate(offset.x(), offset.y());
                    mapping.scale(scale, scale);
                    auto fill = color;
                    fill.setAlphaF(static_cast<float>(style.fillOpacity));
                    painter.setBrush(shape.kind == DataShape::Kind::Polygon ? QBrush(fill)
                                                                            : Qt::NoBrush);
                    painter.drawPath(mapping.map(shape.path));
                    painter.restore();
                }
                if (style.labels && labels < 100 && screen.contains(point) &&
                    !shape.name.isEmpty()) {
                    paintMapLabel(
                        painter,
                        point + (shape.kind == DataShape::Kind::Point
                                     ? view.markerOffset(QPointF(8, -8), style.pointSize)
                                     : QPointF(8, -8)),
                        shape.name,
                        selected && selected->owner == QStringLiteral("org.swak.data/") + id &&
                            selected->id ==
                                QString::number(&shape - feed->document.shapes.constData()));
                    ++labels;
                }
            }
        }
        painter.restore();
    }
    painter.restore();
}
QList<MapItem> DataSources::itemsAt(const Viewport &view, QPointF position) const
{
    QList<MapItem> result;
    const double scale = view.scale();
    const QPointF origin = view.toScreen({0, 0});
    if (!QRectF(origin, QSizeF(scale, scale)).contains(position)) {
        return {};
    }
    auto ids = feeds_.keys();
    ids.sort();
    const QFontMetrics metrics(QGuiApplication::font());
    QMultiHash<const DataShape *, QRectF> labelBounds;
    const QRectF screen(QPointF{}, view.size);
    int labels = 0;
    for (const auto &id : ids) {
        const auto feed = feeds_.value(id);
        if (!feed->style.labels) {
            continue;
        }
        for (const auto &shape : feed->document.shapes) {
            if (labels >= 100) {
                break;
            }
            if (shape.name.isEmpty()) {
                continue;
            }
            for (int wrap = -1; wrap <= 1 && labels < 100; ++wrap) {
                const auto point = shape.point * scale + origin + QPointF(wrap * scale, 0);
                if (!screen.contains(point)) {
                    continue;
                }
                const auto bounds = mapLabelBounds(
                    metrics,
                    point + (shape.kind == DataShape::Kind::Point
                                 ? view.markerOffset(QPointF(8, -8), feed->style.pointSize)
                                 : QPointF(8, -8)),
                    shape.name);
                labelBounds.insert(&shape, bounds);
                ++labels;
            }
        }
    }
    for (auto id = ids.crbegin(); id != ids.crend(); ++id) {
        const auto feed = feeds_.value(*id);
        const auto &source = feed->source;
        if (feed->style.opacity <= 0) {
            continue;
        }
        const double radius = feed->style.pointSize;
        const double lineWidth = feed->style.lineWidth;
        for (auto shape = feed->document.shapes.crbegin(); shape != feed->document.shapes.crend();
             ++shape) {
            for (int wrap = -1; wrap <= 1; ++wrap) {
                const auto offset = origin + QPointF(wrap * scale, 0);
                const auto point = shape->point * scale + offset;
                const auto rectangles = labelBounds.values(&*shape);
                bool hit =
                    std::any_of(rectangles.cbegin(), rectangles.cend(), [&](const QRectF &label) {
                        return label.contains(position);
                    });
                if (shape->kind == DataShape::Kind::Point) {
                    hit = hit || std::hypot(point.x() - position.x(), point.y() - position.y()) <=
                                     std::max(8.0, view.markerSize(radius) + 3);

                } else {
                    const auto bounds = shape->path.boundingRect();
                    if (!hit && !QRectF(bounds.topLeft() * scale + offset, bounds.size() * scale)
                                     .adjusted(-8, -8, 8, 8)
                                     .contains(position)) {
                        continue;
                    }
                    QTransform mapping;
                    mapping.translate(offset.x(), offset.y());
                    mapping.scale(scale, scale);
                    const auto path = mapping.map(shape->path);
                    QPainterPathStroker stroke;
                    stroke.setWidth(std::max(10.0, lineWidth + 6));
                    hit = hit ||
                          (shape->kind == DataShape::Kind::Polygon && path.contains(position)) ||
                          stroke.createStroke(path).contains(position);
                }
                if (!hit) {
                    continue;
                }
                MapItem item;
                item.owner = QStringLiteral("org.swak.data/") + *id;
                item.id = QString::number(&*shape - feed->document.shapes.constData());
                item.kind = shape->kind == DataShape::Kind::Point  ? MapItem::Kind::Point
                            : shape->kind == DataShape::Kind::Line ? MapItem::Kind::Line
                                                                   : MapItem::Kind::Polygon;
                item.type = shape->kind == DataShape::Kind::Point  ? tr("Point")
                            : shape->kind == DataShape::Kind::Line ? tr("Route / track")
                                                                   : tr("Polygon");
                item.label = shape->name.isEmpty() ? item.type : shape->name;
                item.description = shape->description;
                item.properties = shape->properties;
                item.properties.insert(tr("Data provider"), source.value(QStringLiteral("name")));
                item.point = shape->point;
                item.path = shape->path;
                item.color = feed->style.color;
                item.radius = radius;
                item.lineWidth = lineWidth;
                result.append(std::move(item));
                break;
            }
        }
    }
    return result;
}

} // namespace swak

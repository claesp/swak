#include "core/maps.h"
#include <QRegularExpression>
#include <QUuid>
#include <cmath>
#include <algorithm>

namespace swak {
QList<SettingField> commonMapFields()
{
    return {textSetting(QStringLiteral("name"), QObject::tr("Name"), 100),
        textSetting(QStringLiteral("endpoint"), QObject::tr("HTTPS URL"), 2048),
        textSetting(QStringLiteral("attribution"), QObject::tr("Attribution and license URL"), 300),
        numberSetting(QStringLiteral("minZoom"), QObject::tr("Minimum tile zoom"), 0, 19),
        numberSetting(QStringLiteral("maxZoom"), QObject::tr("Maximum tile zoom"), 0, 19),
        booleanSetting(QStringLiteral("ignoreTls"), QObject::tr("Ignore invalid HTTPS certificates (unsafe)"))};
}
QJsonObject mapDefaults(const QString &name, const QString &endpoint, const QString &attribution, int maximumZoom)
{
    return {{QStringLiteral("name"), name}, {QStringLiteral("endpoint"), endpoint},
        {QStringLiteral("attribution"), attribution}, {QStringLiteral("minZoom"), 0},
        {QStringLiteral("maxZoom"), maximumZoom}, {QStringLiteral("ignoreTls"), false}};
}
QString validateMapSource(const QJsonObject &source)
{
    for (const auto &key : {QStringLiteral("name"), QStringLiteral("attribution"), QStringLiteral("endpoint")}) {
        const auto value = source.value(key).toString();
        const int limit = key == QStringLiteral("endpoint") ? 2048 : key == QStringLiteral("name") ? 100 : 300;
        if (value.trimmed().isEmpty() || value.size() > limit) return QObject::tr("Enter a bounded name, URL, and attribution.");
        if (std::any_of(value.begin(), value.end(), [](QChar ch) { return !ch.isPrint(); })) return QObject::tr("Do not use control characters.");
    }
    const QUrl url(source.value(QStringLiteral("endpoint")).toString());
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || url.hasFragment()) return QObject::tr("Use an HTTPS URL without credentials or a fragment.");
    for (const auto &key : {QStringLiteral("minZoom"), QStringLiteral("maxZoom")}) {
        const double value = source.value(key).toDouble(-1);
        if (!std::isfinite(value) || value < 0 || value > 19 || std::floor(value) != value) return QObject::tr("Use integer zoom levels from 0 through 19.");
    }
    if (source.value(QStringLiteral("minZoom")).toInt() > source.value(QStringLiteral("maxZoom")).toInt())
        return QObject::tr("Minimum zoom must not exceed maximum zoom.");
    return {};
}
bool validTile(int zoom, int x, int y)
{
    return zoom >= 0 && zoom <= 19 && x >= 0 && y >= 0 && x < (1 << zoom) && y < (1 << zoom);
}
MapSources::MapSources(const QList<QObject *> &plugins, QObject *parent) : QObject(parent)
{
    QJsonObject entries;
    for (auto *object : plugins) {
        auto *extension = qobject_cast<MapExtension *>(object);
        auto *format = qobject_cast<MapProvider *>(object);
        if (!extension || extension->mapKind() != MapExtension::Kind::Provider || !format) continue;
        providers_.append(format);
        auto source = format->defaults();
        source.insert(QStringLiteral("format"), format->mapId());
        entries.insert(format->mapId(), source);
    }
    values_ = {{QStringLiteral("network"), false}, {QStringLiteral("active"), QString{}}, {QStringLiteral("sources"), entries}};
}
MapProvider *MapSources::provider(const QJsonObject &source) const
{
    const auto it = std::find_if(providers_.begin(), providers_.end(), [&](const auto *format) { return format->mapId() == source.value(QStringLiteral("format")).toString(); });
    return it == providers_.end() ? nullptr : *it;
}
QJsonObject MapSources::activeSource() const
{
    if (!networkEnabled()) return {};
    const auto source = sources().value(activeId()).toObject();
    const auto *format = provider(source);
    return format && format->validate(source).isEmpty() ? source : QJsonObject{};
}
QList<SettingField> MapSources::settingsFields() const
{
    QStringList names{tr("Offline reference map")};
    QJsonArray ids{QString{}};
    const auto entries = sources();
    for (auto it = entries.begin(); it != entries.end(); ++it) { names.append(it.value().toObject().value(QStringLiteral("name")).toString()); ids.append(it.key()); }
    QList<SettingField> fields{booleanSetting(QStringLiteral("network"), tr("Enable online maps")),
        choiceSetting(QStringLiteral("active"), tr("Active provider"), names, ids)};
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const auto source = it.value().toObject();
        if (const auto *format = provider(source)) {
            for (auto field : format->fields()) {
                field.path = QStringLiteral("sources/") + it.key() + QLatin1Char('/') + field.path;
                field.label = source.value(QStringLiteral("name")).toString() + QStringLiteral(": ") + field.label;
                fields.append(field);
            }
        }
    }
    return fields;
}
QString MapSources::validateSettings(const QJsonObject &values) const
{
    const auto entries = values.value(QStringLiteral("sources")).toObject();
    if (entries.size() > 32) return tr("Use at most 32 providers.");
    static const QRegularExpression idPattern(QStringLiteral("^[a-zA-Z0-9.-]{1,100}$"));
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!idPattern.match(it.key()).hasMatch()) return tr("The provider identifier is invalid.");
        const auto source = it.value().toObject();
        if (const auto *format = provider(source)) {
            const auto error = format->validate(source);
            if (!error.isEmpty()) return error;
        }
    }
    return {};
}
void MapSources::restorePreferences(const QJsonObject &values)
{
    if (!validateSettings(values).isEmpty()) return;
    values_ = values;
    emit changed();
}
QString MapSources::put(const QString &id, const QJsonObject &source)
{
    auto next = values_;
    auto entries = sources();
    entries.insert(id, source);
    next.insert(QStringLiteral("sources"), entries);
    const auto error = validateSettings(next);
    if (error.isEmpty()) restorePreferences(next);
    return error;
}
void MapSources::remove(const QString &id)
{
    auto entries = sources();
    entries.remove(id);
    values_.insert(QStringLiteral("sources"), entries);
    if (activeId() == id) values_.insert(QStringLiteral("active"), QString{});
    emit changed();
}
void MapSources::select(const QString &id)
{
    values_.insert(QStringLiteral("active"), id);
    values_.insert(QStringLiteral("network"), !id.isEmpty());
    emit changed();
}
QString MapSources::add(const QString &format)
{
    for (const auto *entry : providers_) if (entry->mapId() == format && sources().size() < 32) {
        auto source = entry->defaults();
        source.insert(QStringLiteral("format"), format);
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (put(id, source).isEmpty()) return id;
    }
    return {};
}
}

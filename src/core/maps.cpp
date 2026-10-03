#include "core/maps.h"
#include "core/logging.h"
#include <QRegularExpression>
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
        choiceSetting(QStringLiteral("authMode"), QObject::tr("Authentication"),
            {QObject::tr("None"), QObject::tr("Basic username and password"), QObject::tr("Bearer token"), QObject::tr("API key header"), QObject::tr("API key query")},
            {QStringLiteral("none"), QStringLiteral("basic"), QStringLiteral("bearer"), QStringLiteral("header"), QStringLiteral("query")}),
        textSetting(QStringLiteral("authUser"), QObject::tr("Authentication username"), 256),
        textSetting(QStringLiteral("authParameter"), QObject::tr("API key parameter or X- header"), 64),
        textSetting(QStringLiteral("authEnvironment"), QObject::tr("Secret environment variable (SWAK_MAP_...)"), 128),
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
    return validateMapAuthentication(source);
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
    QJsonArray known; for (const auto *format : providers_) known.append(format->mapId());
    values_ = {{QStringLiteral("knownProviders"), known}, {QStringLiteral("network"), false}, {QStringLiteral("active"), QString{}}, {QStringLiteral("sources"), entries}};
    addNewPresets();
}
void MapSources::addNewPresets()
{
    static const QRegularExpression idPattern(QStringLiteral("\\A[a-zA-Z0-9.-]{1,100}\\z"));
    auto entries = sources();
    auto known = values_.value(QStringLiteral("knownPresets")).toArray();
    for (const auto *format : providers_) {
        const auto presets = format->additionalSources();
        for (auto it = presets.begin(); it != presets.end(); ++it) {
            if (!idPattern.match(it.key()).hasMatch() || !it.value().isObject()) continue;
            if (known.contains(it.key()) || known.size() >= 256) continue;
            if (!entries.contains(it.key())) {
                if (entries.size() >= 32) continue;
                auto source = it.value().toObject();
                source.insert(QStringLiteral("format"), format->mapId());
                if (!format->validate(source).isEmpty()) continue;
                entries.insert(it.key(), source);
            }
            known.append(it.key());
        }
    }
    values_.insert(QStringLiteral("knownPresets"), known);
    values_.insert(QStringLiteral("sources"), entries);
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
    QStringList names{tr("No map provider")};
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
    static const QRegularExpression idPattern(QStringLiteral("\\A[a-zA-Z0-9.-]{1,100}\\z"));
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!idPattern.match(it.key()).hasMatch()) return tr("The provider identifier is invalid.");
        const auto source = it.value().toObject();
        if (const auto *format = provider(source); format && sources().value(it.key()) != it.value()) {
            const auto error = format->validate(source);
            if (!error.isEmpty()) return error;
        }
    }
    return {};
}
void MapSources::restorePreferences(const QJsonObject &values)
{
    auto structural = values;
    auto entries = structural.value(QStringLiteral("sources")).toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it) it.value() = QJsonObject{};
    structural.insert(QStringLiteral("sources"), entries);
    if (!validateSettings(structural).isEmpty()) return;
    const auto oldSources = sources();
    for (const auto &id : credentials_.keys()) {
        const auto before = oldSources.value(id).toObject();
        const auto after = values.value(QStringLiteral("sources")).toObject().value(id).toObject();
        for (const auto *key : {"endpoint", "authMode", "authUser", "authParameter", "authEnvironment", "ignoreTls"})
            if (before.value(QLatin1String(key)) != after.value(QLatin1String(key))) { credentials_.remove(id); break; }
    }
    values_ = values;
    auto known = values.value(QStringLiteral("knownProviders")).toArray();
    if (!values.contains(QStringLiteral("knownProviders")))
        known = {QStringLiteral("org.swak.xyz"), QStringLiteral("org.swak.wmts"), QStringLiteral("org.swak.wms")};
    auto restored = sources();
    for (const auto *format : providers_) if (!known.contains(format->mapId())) {
        known.append(format->mapId());
        if (!restored.contains(format->mapId()) && restored.size() < 32) {
            auto source = format->defaults(); source.insert(QStringLiteral("format"), format->mapId());
            restored.insert(format->mapId(), source);
        }
    }
    values_.insert(QStringLiteral("sources"), restored);
    values_.insert(QStringLiteral("knownProviders"), known);
    addNewPresets();
    for (const auto &entry : sources()) {
        const auto source = entry.toObject();
        if (const auto *format = provider(source); format && !format->validate(source).isEmpty())
            logMessage(Severity::Warning, Urgency::Attention, QStringLiteral("swak.maps"),
                tr("An invalid map provider is disabled. Its configuration is preserved."));
    }
    emit changed();
}
QString MapSources::put(const QString &id, const QJsonObject &source)
{
    if (const auto *format = provider(source)) {
        const auto error = format->validate(source);
        if (!error.isEmpty()) return error;
    }
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
    credentials_.remove(id);
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
}

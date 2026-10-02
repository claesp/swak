#include "core/plugin_catalog.h"
#include "core/interfaces.h"
#include "core/maps.h"
#include "core/data.h"
#include "core/logging.h"
#include <QJsonArray>
#include <QPluginLoader>
#include <QRegularExpression>
#include <QSet>
#include <QVersionNumber>
#include <algorithm>

namespace swak {
QString validatePluginManifest(const QJsonObject &manifest, QObject *instance)
{
    const auto *plugin = qobject_cast<Plugin *>(instance);
    if (!plugin || plugin->apiVersion() != 2 || manifest.value(QStringLiteral("apiVersion")).toInt() != 2
        || manifest.value(QStringLiteral("schemaVersion")).toInt() != 1)
        return QStringLiteral("Unsupported plugin API or manifest schema.");
    static const QRegularExpression identifier(QStringLiteral("\\Aorg\\.swak\\.[a-z][a-z0-9.-]{0,80}\\z"));
    static const QRegularExpression version(QStringLiteral("\\A[0-9]{1,4}\\.[0-9]{1,4}\\.[0-9]{1,4}\\z"));
    if (!identifier.match(manifest.value(QStringLiteral("id")).toString()).hasMatch()
        || !version.match(manifest.value(QStringLiteral("version")).toString()).hasMatch())
        return QStringLiteral("Invalid plugin identifier or version.");
    for (const auto *field : {"name", "description"}) {
        const auto text = manifest.value(QLatin1String(field)).toString();
        if (text.trimmed().isEmpty() || text.size() > 512) return QStringLiteral("Missing or oversized plugin description.");
    }
    if (manifest.value(QStringLiteral("linkage")).toString() != QStringLiteral("static")
        || manifest.value(QStringLiteral("qtMajor")).toInt() != QT_VERSION_MAJOR
        || !manifest.value(QStringLiteral("platforms")).toArray().contains(QStringLiteral("linux"))
        || manifest.value(QStringLiteral("entryPoint")).toString() != QString::fromLatin1(instance->metaObject()->className()).section(QStringLiteral("::"), -1))
        return QStringLiteral("Incompatible plugin entry point or platform.");
    const auto minimum = manifest.value(QStringLiteral("qtMinimum")).toString();
    if (!version.match(minimum).hasMatch() || QVersionNumber::fromString(minimum) > QVersionNumber::fromString(QString::fromLatin1(qVersion())))
        return QStringLiteral("Unsupported Qt version.");
    QSet<QString> roles;
    if (qobject_cast<Codec *>(instance)) roles.insert(QStringLiteral("codec"));
    if (qobject_cast<MapLayer *>(instance)) roles.insert(QStringLiteral("map-layer"));
    if (qobject_cast<ViewPlugin *>(instance)) roles.insert(QStringLiteral("view"));
    if (qobject_cast<ToolPlugin *>(instance)) roles.insert(QStringLiteral("tool"));
    if (qobject_cast<SourcePlugin *>(instance)) roles.insert(QStringLiteral("source"));
    if (qobject_cast<PanePlugin *>(instance)) roles.insert(QStringLiteral("pane"));
    if (qobject_cast<MenuPlugin *>(instance)) roles.insert(QStringLiteral("menu"));
    if (qobject_cast<ToolbarPlugin *>(instance)) roles.insert(QStringLiteral("toolbar"));
    if (auto *extension = qobject_cast<MapExtension *>(instance)) {
        if (extension->mapId() != manifest.value(QStringLiteral("id")).toString()) return QStringLiteral("Map plugin identifier mismatch.");
        roles.insert(extension->mapKind() == MapExtension::Kind::Provider ? QStringLiteral("map-provider") : QStringLiteral("map-tool"));
        if (extension->mapKind() == MapExtension::Kind::Provider && !qobject_cast<MapProvider *>(instance)) return QStringLiteral("Missing map provider interface.");
    }
    if (auto *provider = qobject_cast<DataProvider *>(instance)) {
        if (provider->dataId() != manifest.value(QStringLiteral("id")).toString()) return QStringLiteral("Data plugin identifier mismatch.");
        roles.insert(QStringLiteral("data-provider"));
    }
    const auto declared = manifest.value(QStringLiteral("interfaces")).toArray();
    if (declared.isEmpty() || declared.size() != roles.size()) return QStringLiteral("Plugin interface manifest mismatch.");
    if (std::any_of(declared.cbegin(), declared.cend(), [&roles](const QJsonValue &role) { return !roles.remove(role.toString()); }))
        return QStringLiteral("Plugin interface manifest mismatch.");
    if (!manifest.value(QStringLiteral("requires")).isArray() || !manifest.value(QStringLiteral("permissions")).isArray()
        || manifest.value(QStringLiteral("settingsSchemaVersion")).toInt() != 1) return QStringLiteral("Invalid plugin requirements or preferences schema.");
    const auto requirements = manifest.value(QStringLiteral("requires")).toArray();
    const bool needsCodec = qobject_cast<ToolPlugin *>(instance) || qobject_cast<SourcePlugin *>(instance);
    if (requirements != (needsCodec ? QJsonArray{QStringLiteral("codec")} : QJsonArray{})) return QStringLiteral("Plugin requirements do not match its interfaces.");
    const QSet<QString> permissions{QStringLiteral("network-udp"), QStringLiteral("network-https"), QStringLiteral("disk-cache"),
        QStringLiteral("browser-links"), QStringLiteral("user-selected-files")};
    QSet<QString> seen;
    for (const auto &permission : manifest.value(QStringLiteral("permissions")).toArray()) {
        const auto value = permission.toString();
        if (!permissions.contains(value) || seen.contains(value)) return QStringLiteral("Unknown or duplicate plugin permission.");
        seen.insert(value);
    }
    return {};
}
QList<QObject *> builtInPlugins()
{
    QList<QObject *> result;
    QSet<QString> ids;
    for (const auto &entry : QPluginLoader::staticPlugins()) {
        const auto metadata = entry.metaData();
        if (metadata.value(QStringLiteral("IID")).toString() != QStringLiteral("org.swak.Plugin/2.0")) continue;
        auto *instance = entry.instance();
        const auto manifest = metadata.value(QStringLiteral("MetaData")).toObject();
        const auto error = validatePluginManifest(manifest, instance);
        const auto id = manifest.value(QStringLiteral("id")).toString();
        if (!error.isEmpty() || ids.contains(id)) {
            logMessage(Severity::Error, Urgency::Attention, QStringLiteral("swak.plugins"),
                error.isEmpty() ? QStringLiteral("Duplicate plugin identifier.") : error);
            continue;
        }
        ids.insert(id);
        result.append(instance);
    }
    return result;
}
}

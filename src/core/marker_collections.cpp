#include "core/marker_collections.h"
#include "core/marker_symbols.h"
#include "core/files.h"
#include "core/logging.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QPainter>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cmath>
#include <algorithm>
#include <QSet>
#include <memory>
namespace swak {
namespace {
constexpr qsizetype ArchiveLimit = 1024 * 1024;
constexpr qsizetype CatalogLimit = 256 * 1024;
bool textValid(const QJsonValue &value, int limit, bool empty = false)
{
    if (!value.isString()) return false;
    const auto text = value.toString();
    return text.size() <= limit && (empty || !text.trimmed().isEmpty()) &&
        std::all_of(text.cbegin(), text.cend(), [](QChar c) { return c.isPrint(); });
}
bool identifier(const QString &value, bool empty = false)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[a-zA-Z0-9.-]{1,64}\\z"));
    return (empty && value.isEmpty()) || pattern.match(value).hasMatch();
}
std::optional<QPainterPath> decodePath(const QJsonValue &value, bool empty)
{
    if (!value.isArray()) return {};
    const auto commands = value.toArray();
    if (commands.size() > 64 || (!empty && commands.isEmpty())) return {};
    QPainterPath path;
    if (commands.isEmpty()) return path;
    for (qsizetype i = 0; i < commands.size(); ++i) {
        const auto command = commands.at(i).toArray();
        if (command.isEmpty()) return {};
        const auto kind = command.first().toString();
        const int count = kind == QStringLiteral("C") ? 7 : (kind == QStringLiteral("M") || kind == QStringLiteral("L")) ? 3 : 0;
        if (!count || command.size() != count || (i == 0 && kind != QStringLiteral("M"))) return {};
        for (int n = 1; n < count; ++n)
            if (!command[n].isDouble() || !std::isfinite(command[n].toDouble()) || std::abs(command[n].toDouble()) > 2) return {};
        if (kind == QStringLiteral("M")) path.moveTo(command[1].toDouble(), command[2].toDouble());
        else if (kind == QStringLiteral("L")) path.lineTo(command[1].toDouble(), command[2].toDouble());
        else path.cubicTo(command[1].toDouble(), command[2].toDouble(), command[3].toDouble(), command[4].toDouble(), command[5].toDouble(), command[6].toDouble());
    }
    if (!QRectF(-1.01, -1.01, 2.02, 2.02).contains(path.boundingRect())) return {};
    if (!empty && (path.boundingRect().width() < 0.1 || path.boundingRect().height() < 0.1)) return {};
    return path;
}
CollectionSymbol decodeSymbol(const QJsonObject &value)
{
    return {value.value(QStringLiteral("id")).toString(), value.value(QStringLiteral("name")).toString(),
        QColor(value.value(QStringLiteral("color")).toString()), *decodePath(value.value(QStringLiteral("frame")), false),
        *decodePath(value.value(QStringLiteral("glyph")), true)};
}
QString key(const QString &collection, const QString &id) { return collection + QLatin1Char('/') + id; }
}
QJsonArray encodeMarkerPath(const QPainterPath &path)
{
    QJsonArray result;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto element = path.elementAt(i);
        if (element.isCurveTo() && i + 2 < path.elementCount()) {
            const auto control = path.elementAt(++i); const auto end = path.elementAt(++i);
            result.append(QJsonArray{QStringLiteral("C"), element.x, element.y, control.x, control.y, end.x, end.y});
        } else result.append(QJsonArray{element.isMoveTo() ? QStringLiteral("M") : QStringLiteral("L"), element.x, element.y});
    }
    return result;
}
QJsonObject coreMarkerCollection()
{
    return {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("id"), CoreMarkerCollection}, {QStringLiteral("name"), QObject::tr("Core")},
        {QStringLiteral("description"), QObject::tr("Built-in SWAK markers.")}, {QStringLiteral("version"), QStringLiteral("1.0.0")}, {QStringLiteral("enabled"), true},
        {QStringLiteral("symbols"), QJsonArray{QJsonObject{{QStringLiteral("id"), QString{}}, {QStringLiteral("name"), QObject::tr("Generic marker")},
            {QStringLiteral("color"), QStringLiteral("#ffc857")}, {QStringLiteral("frame"), encodeMarkerPath(markerFrame({}, {}, 1))}, {QStringLiteral("glyph"), QJsonArray{}}}}}};
}
QString effectiveMarkerCollection(const QString &collection, const QString &symbol)
{ return collection.isEmpty() ? (symbol.isEmpty() ? CoreMarkerCollection : MilitaryMarkerCollection) : collection; }
void paintCollectionSymbol(QPainter &painter, const CollectionSymbol &symbol, QPointF center, double radius, const QColor &color)
{
    QTransform transform; transform.translate(center.x(), center.y()); transform.scale(radius, radius);
    painter.save(); painter.setPen(QPen(QColor("#152028"), 2)); painter.setBrush(color); painter.drawPath(transform.map(symbol.frame));
    painter.setPen(QPen(QColor("#152028"), 1)); painter.setBrush(Qt::NoBrush); painter.drawPath(transform.map(symbol.glyph)); painter.restore();
}
MarkerCollections::MarkerCollections(const QList<QObject *> &plugins, QObject *parent) : QObject(parent)
{
    for (auto *plugin : plugins) if (auto *provider = qobject_cast<MarkerCollectionProvider *>(plugin)) {
        const auto value = provider->markerCollection();
        if (validate(value).isEmpty()) builtins_.insert(value.value(QStringLiteral("id")).toString(), value);
    }
    entries_ = builtins_; rebuild();
}
QString MarkerCollections::validate(const QJsonObject &value)
{
    if (value.value(QStringLiteral("schemaVersion")).toInt() != 1 || !identifier(value.value(QStringLiteral("id")).toString()) ||
        !textValid(value.value(QStringLiteral("name")), 100) || !textValid(value.value(QStringLiteral("description")), 1024, true) ||
        !textValid(value.value(QStringLiteral("version")), 32) || !value.value(QStringLiteral("enabled")).isBool() || !value.value(QStringLiteral("symbols")).isArray())
        return tr("Invalid collection metadata.");
    const auto symbols = value.value(QStringLiteral("symbols")).toArray();
    if (symbols.isEmpty() || symbols.size() > 64 || QJsonDocument(value).toJson(QJsonDocument::Compact).size() > CatalogLimit)
        return tr("Use 1 through 64 symbols within the collection size limit.");
    QSet<QString> ids;
    static const QRegularExpression color(QStringLiteral("\\A#[0-9a-fA-F]{6}\\z"));
    for (const auto &entry : symbols) {
        const auto definition = entry.toObject(); const auto id = definition.value(QStringLiteral("id"));
        if (!id.isString() || !identifier(id.toString(), true) || ids.contains(id.toString()) ||
            !textValid(definition.value(QStringLiteral("name")), 100) || !color.match(definition.value(QStringLiteral("color")).toString()).hasMatch() ||
            !decodePath(definition.value(QStringLiteral("frame")), false) || !decodePath(definition.value(QStringLiteral("glyph")), true))
            return tr("Invalid symbol name, identifier, color, or vector path.");
        ids.insert(id.toString());
    }
    return {};
}
void MarkerCollections::rebuild()
{
    symbols_.clear();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        const auto value = it.value().toObject(); if (!validate(value).isEmpty()) continue;
        for (const auto &entry : value.value(QStringLiteral("symbols")).toArray()) {
            const auto item = decodeSymbol(entry.toObject()); symbols_.insert(key(it.key(), item.id), item);
        }
    }
}
std::optional<CollectionSymbol> MarkerCollections::symbol(const QString &collection, const QString &id) const
{
    const auto found = symbols_.constFind(key(effectiveMarkerCollection(collection, id), id));
    return found == symbols_.cend() ? std::optional<CollectionSymbol>{} : *found;
}
QJsonObject MarkerCollections::savePreferences() const
{ return {{QStringLiteral("collections"), entries_}, {QStringLiteral("directory"), directory_}}; }
void MarkerCollections::restorePreferences(const QJsonObject &values)
{
    const auto entries = values.value(QStringLiteral("collections")).toObject();
    if (entries.size() > 16 || QJsonDocument(entries).toJson(QJsonDocument::Compact).size() > CatalogLimit) return;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it.key() != it.value().toObject().value(QStringLiteral("id")).toString() || !identifier(it.key())) return;
    }
    auto restored = entries;
    for (auto it = builtins_.begin(); it != builtins_.end(); ++it) {
        auto value = it.value().toObject();
        value.insert(QStringLiteral("enabled"), restored.value(it.key()).toObject().value(QStringLiteral("enabled")).toBool(true));
        restored.insert(it.key(), value);
    }
    if (restored.size() > 16 || QJsonDocument(restored).toJson(QJsonDocument::Compact).size() > CatalogLimit) return;
    entries_ = restored;
    directory_ = values.value(QStringLiteral("directory")).toString().left(4096);
    rebuild(); emit changed();
}
QString MarkerCollections::put(const QJsonObject &value)
{
    const auto id = value.value(QStringLiteral("id")).toString();
    if (builtIn(id)) return tr("Built-in collections are read-only. Add a copy to change them.");
    const auto error = validate(value); if (!error.isEmpty()) return error;
    auto next = entries_; next.insert(id, value);
    if (next.size() > 16 || QJsonDocument(next).toJson(QJsonDocument::Compact).size() > CatalogLimit) return tr("The collection catalog is full.");
    entries_ = next; rebuild(); emit changed(); return {};
}
QString MarkerCollections::remove(const QString &id)
{
    if (builtIn(id)) return tr("Built-in collections cannot be deleted. Disable the collection instead.");
    entries_.remove(id); rebuild(); emit changed(); return {};
}
QString MarkerCollections::setEnabled(const QString &id, bool enabled)
{
    if (!entries_.contains(id)) return tr("The collection is unavailable.");
    auto value = entries_.value(id).toObject(); const auto error = validate(value); if (!error.isEmpty()) return error;
    value.insert(QStringLiteral("enabled"), enabled); entries_.insert(id, value); emit changed(); return {};
}
QString MarkerCollections::importArchive(const QString &path)
{
    const auto file = readRegularFile(path, ArchiveLimit); if (!file.error.isEmpty()) return file.error;
    std::unique_ptr<archive, decltype(&archive_read_free)> input(archive_read_new(), archive_read_free);
    if (!input) return tr("Cannot allocate an archive reader.");
    if (archive_read_support_filter_gzip(input.get()) != ARCHIVE_OK) return tr("Gzip support is unavailable.");
    archive_read_support_format_zip(input.get()); archive_read_support_format_tar(input.get());
    if (archive_read_open_memory(input.get(), file.bytes.constData(), static_cast<size_t>(file.bytes.size())) != ARCHIVE_OK) return tr("Cannot open the collection archive.");
    archive_entry *entry = nullptr;
    if (archive_read_next_header(input.get(), &entry) != ARCHIVE_OK || !entry) return tr("The archive has no collection.json file.");
    const auto *name = archive_entry_pathname_utf8(entry);
    if (!name || QByteArray(name) != QByteArrayLiteral("collection.json") || archive_entry_filetype(entry) != AE_IFREG ||
        archive_entry_symlink(entry) || archive_entry_hardlink(entry) || archive_entry_is_encrypted(entry) ||
        archive_entry_size(entry) < 0 || archive_entry_size(entry) > CatalogLimit) return tr("The archive must contain only a regular collection.json file.");
    QByteArray bytes; std::array<char, 4096> buffer{}; la_ssize_t count = 0;
    while ((count = archive_read_data(input.get(), buffer.data(), buffer.size())) > 0) {
        if (bytes.size() + count > CatalogLimit) return tr("The expanded collection exceeds the size limit.");
        bytes.append(buffer.data(), static_cast<qsizetype>(count));
    }
    if (count < 0 || archive_read_next_header(input.get(), &entry) != ARCHIVE_EOF) return tr("The archive is damaged or contains extra entries.");
    QJsonParseError parse; const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) return tr("The collection JSON is invalid.");
    const auto value = document.object();
    if (entries_.contains(value.value(QStringLiteral("id")).toString())) return tr("A collection with this identifier already exists.");
    return put(value);
}
QString MarkerCollections::exportArchive(const QString &id, const QString &path) const
{
    if (path.contains(QChar::Null)) return tr("The export path contains a null character.");
    const auto value = entries_.value(id).toObject(); const auto error = validate(value); if (!error.isEmpty()) return error;
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    std::unique_ptr<archive, decltype(&archive_write_free)> output(archive_write_new(), archive_write_free);
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
    if (!output || !entry) return tr("Cannot allocate an archive writer.");
    int result;
    if (path.endsWith(QLatin1String(".tar.gz"), Qt::CaseInsensitive)) {
        result = archive_write_set_format_pax_restricted(output.get());
        if (result == ARCHIVE_OK) result = archive_write_add_filter_gzip(output.get());
    } else result = archive_write_set_format_zip(output.get());
    QByteArray data(ArchiveLimit, '\0'); size_t used = 0;
    if (result != ARCHIVE_OK || archive_write_open_memory(output.get(), data.data(), static_cast<size_t>(data.size()), &used) != ARCHIVE_OK) return tr("Cannot create the collection archive.");
    archive_entry_set_pathname(entry.get(), "collection.json"); archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0600); archive_entry_set_size(entry.get(), bytes.size());
    if (archive_write_header(output.get(), entry.get()) != ARCHIVE_OK || archive_write_data(output.get(), bytes.constData(), static_cast<size_t>(bytes.size())) != bytes.size() || archive_write_close(output.get()) != ARCHIVE_OK)
        return tr("Cannot finish the collection archive.");
    QSaveFile file(path); if (!file.open(QIODevice::WriteOnly)) return tr("Cannot open the export file.");
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return tr("Cannot set private export permissions.");
    if (file.write(data.constData(), static_cast<qint64>(used)) != static_cast<qint64>(used) || !file.commit()) return tr("Cannot save the collection archive.");
    return {};
}
}

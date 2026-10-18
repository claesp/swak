#include "core/data_package.h"

#include "core/text.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <memory>

namespace swak {
namespace {
const QString ManifestPath = QStringLiteral("MANIFEST/manifest.xml");
constexpr qsizetype MaxManifestBytes = 256 * 1024;
constexpr int MaxManifestContents = 1024;

QString tr(const char *text)
{
    return QCoreApplication::translate("DataPackage", text);
}
QString plain(const QString &text)
{
    return plainText(text, 128);
}
struct Manifest
{
    QString uid;
    QString name;
    bool hasContents = false;
    QStringList included;
    QSet<QString> ignored;
    QString error;
};
Manifest readManifest(const QByteArray &bytes)
{
    Manifest manifest;
    QXmlStreamReader xml(bytes);
    bool configuration = false;
    bool contents = false;
    int depth = 0;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference) {
            manifest.error = tr("The package manifest contains a document type.");
            return manifest;
        }
        if (token == QXmlStreamReader::EndElement) {
            --depth;
            if (xml.name() == u"Configuration") {
                configuration = false;
            } else if (xml.name() == u"Contents") {
                contents = false;
            }
            continue;
        }
        if (token != QXmlStreamReader::StartElement) {
            continue;
        }
        if (++depth > 8) {
            manifest.error = tr("The package manifest is nested too deeply.");
            return manifest;
        }
        const auto name = xml.name();
        const auto attributes = xml.attributes();
        if (name == u"Configuration") {
            configuration = true;
        } else if (name == u"Contents") {
            contents = true;
            manifest.hasContents = true;
        } else if (configuration && name == u"Parameter") {
            const auto key = attributes.value(u"name");
            if (key == u"uid") {
                manifest.uid = plain(attributes.value(u"value").toString());
            } else if (key == u"name") {
                manifest.name = plain(attributes.value(u"value").toString());
            }
        } else if (contents && name == u"Content") {
            if (manifest.included.size() + manifest.ignored.size() >= MaxManifestContents) {
                manifest.error = tr("The package manifest lists too many files.");
                return manifest;
            }
            const auto path = attributes.value(u"zipEntry").toString();
            if (attributes.value(u"ignore").compare(u"true", Qt::CaseInsensitive) == 0) {
                manifest.ignored.insert(path);
            } else {
                manifest.included.append(path);
            }
        }
    }
    if (xml.hasError()) {
        manifest.error = tr("The package manifest is not valid XML.");
    }
    return manifest;
}
} // namespace

bool safePackagePath(const QString &path)
{
    if (path.isEmpty() || path.size() > 255 || path.startsWith(QLatin1Char('/')) ||
        path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':'))) {
        return false;
    }
    if (std::any_of(path.cbegin(), path.cend(), [](QChar ch) {
            return ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format;
        })) {
        return false;
    }
    const auto segments = path.split(QLatin1Char('/'));
    return std::none_of(segments.cbegin(), segments.cend(), [](const QString &segment) {
        return segment.isEmpty() || segment == u"." || segment == u"..";
    });
}
DataPackage readDataPackage(const QByteArray &zip)
{
    DataPackage package;
    if (zip.isEmpty() || zip.size() > MaxPackageBytes) {
        package.error = tr("The package is empty or larger than 64 MiB.");
        return package;
    }
    std::unique_ptr<archive, decltype(&archive_read_free)> input(archive_read_new(),
                                                                 archive_read_free);
    if (!input || archive_read_support_format_zip(input.get()) != ARCHIVE_OK ||
        archive_read_open_memory(input.get(), zip.constData(), static_cast<size_t>(zip.size())) !=
            ARCHIVE_OK) {
        package.error = tr("The package is not a zip archive.");
        return package;
    }
    QHash<QString, QByteArray> files;
    QStringList order;
    qsizetype expanded = 0;
    int count = 0;
    archive_entry *entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(input.get(), &entry)) == ARCHIVE_OK) {
        if (++count > MaxPackageEntries) {
            package.error = tr("The package has more than %1 entries.").arg(MaxPackageEntries);
            return package;
        }
        const auto *raw = archive_entry_pathname_utf8(entry);
        auto path = raw ? QString::fromUtf8(raw) : QString{};
        const auto type = archive_entry_filetype(entry);
        if (type == AE_IFDIR) {
            continue;
        }
        if (type != AE_IFREG || archive_entry_symlink(entry) || archive_entry_hardlink(entry) ||
            archive_entry_is_encrypted(entry)) {
            package.error = tr("The package contains a link, a special file, or encryption.");
            return package;
        }
        if (!safePackagePath(path) || files.contains(path)) {
            package.error = tr("The package contains an unsafe or repeated path.");
            return package;
        }
        QByteArray bytes;
        std::array<char, 16 * 1024> buffer{};
        la_ssize_t read = 0;
        while ((read = archive_read_data(input.get(), buffer.data(), buffer.size())) > 0) {
            expanded += read;
            if (bytes.size() + read > MaxPackageEntryBytes || expanded > MaxPackageExpansion) {
                package.error = tr("The expanded package exceeds the size limit.");
                return package;
            }
            bytes.append(buffer.data(), static_cast<qsizetype>(read));
        }
        if (read < 0) {
            package.error = tr("The package archive is damaged.");
            return package;
        }
        files.insert(path, bytes);
        order.append(path);
    }
    if (status != ARCHIVE_EOF) {
        package.error = tr("The package archive is damaged.");
        return package;
    }
    Manifest manifest;
    const auto manifestFile = files.constFind(ManifestPath);
    if (manifestFile != files.cend()) {
        if (manifestFile->size() > MaxManifestBytes) {
            package.error = tr("The package manifest is too large.");
            return package;
        }
        manifest = readManifest(*manifestFile);
        if (!manifest.error.isEmpty()) {
            package.error = manifest.error;
            return package;
        }
    }
    package.uid = manifest.uid;
    package.name = manifest.name;
    const auto &paths = manifest.hasContents ? manifest.included : order;
    QSet<QString> added;
    for (const auto &path : paths) {
        // A manifest can name files that the archive does not contain. Skip them.
        if (path == ManifestPath || manifest.ignored.contains(path) || added.contains(path) ||
            !files.contains(path)) {
            continue;
        }
        added.insert(path);
        package.entries.append({path, files.value(path)});
    }
    return package;
}
QByteArray writeDataPackage(const QString &uid,
                            const QString &name,
                            const QList<PackageEntry> &entries,
                            QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return QByteArray{};
    };
    if (entries.size() >= MaxPackageEntries || plain(uid).isEmpty() || plain(uid) != uid ||
        plain(name) != name) {
        return fail(tr("The package has an invalid name, UID, or entry count."));
    }
    qsizetype total = 0;
    QSet<QString> paths;
    for (const auto &entry : entries) {
        if (!safePackagePath(entry.path) || entry.path == ManifestPath ||
            paths.contains(entry.path) || entry.bytes.size() > MaxPackageEntryBytes) {
            return fail(tr("The package contains an unsafe, repeated, or large file."));
        }
        paths.insert(entry.path);
        total += entry.bytes.size();
    }
    if (total > MaxPackageBytes) {
        return fail(tr("The package is larger than 64 MiB."));
    }
    QByteArray manifest;
    QXmlStreamWriter xml(&manifest);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("MissionPackageManifest"));
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("2"));
    xml.writeStartElement(QStringLiteral("Configuration"));
    for (const auto &[key, value] :
         {std::pair{QStringLiteral("uid"), uid}, std::pair{QStringLiteral("name"), name}}) {
        xml.writeEmptyElement(QStringLiteral("Parameter"));
        xml.writeAttribute(QStringLiteral("name"), key);
        xml.writeAttribute(QStringLiteral("value"), value);
    }
    xml.writeEndElement();
    xml.writeStartElement(QStringLiteral("Contents"));
    for (const auto &entry : entries) {
        xml.writeEmptyElement(QStringLiteral("Content"));
        xml.writeAttribute(QStringLiteral("ignore"), QStringLiteral("false"));
        xml.writeAttribute(QStringLiteral("zipEntry"), entry.path);
    }
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    // Deflate can grow incompressible data a little. Leave room for that and the headers.
    const auto content = total + manifest.size();
    QByteArray output(content + content / 64 + (entries.size() + 1) * 1024 + 64 * 1024, '\0');
    size_t used = 0;
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(),
                                                                   archive_write_free);
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)> header(archive_entry_new(),
                                                                         archive_entry_free);
    if (!writer || !header || archive_write_set_format_zip(writer.get()) != ARCHIVE_OK ||
        // Java ZipInputStream, which TAK Server and ATAK use, rejects stored entries with data
        // descriptors. Deflated entries work with every reader.
        archive_write_set_options(writer.get(), "zip:compression=deflate") != ARCHIVE_OK ||
        archive_write_open_memory(
            writer.get(), output.data(), static_cast<size_t>(output.size()), &used) != ARCHIVE_OK) {
        return fail(tr("Cannot create the package archive."));
    }
    QList<PackageEntry> all{{ManifestPath, manifest}};
    all.append(entries);
    for (const auto &entry : std::as_const(all)) {
        archive_entry_clear(header.get());
        const auto path = entry.path.toUtf8();
        archive_entry_set_pathname_utf8(header.get(), path.constData());
        archive_entry_set_filetype(header.get(), AE_IFREG);
        archive_entry_set_perm(header.get(), 0644);
        archive_entry_set_size(header.get(), entry.bytes.size());
        if (archive_write_header(writer.get(), header.get()) != ARCHIVE_OK ||
            archive_write_data(writer.get(),
                               entry.bytes.constData(),
                               static_cast<size_t>(entry.bytes.size())) != entry.bytes.size()) {
            return fail(tr("Cannot write the package archive."));
        }
    }
    if (archive_write_close(writer.get()) != ARCHIVE_OK) {
        return fail(tr("Cannot finish the package archive."));
    }
    output.truncate(static_cast<qsizetype>(used));
    return output;
}
} // namespace swak

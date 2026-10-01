#include "core/interfaces.h"
#include <QObject>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <cmath>

namespace swak {
class CotPlugin final : public QObject, public Codec {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.swak.Codec/1.0")
    Q_INTERFACES(swak::Codec)
public:
    QString name() const override { return tr("CoT XML 2.0"); }
    DecodeResult decode(const QByteArray &data) const override
    {
        const auto fail = [](const QString &error) { return DecodeResult{std::nullopt, error}; };
        if (data.isEmpty() || data.size() > MaxMessageBytes) return fail(tr("The XML message must contain 1 to 16384 bytes."));
        if (data.contains('\0')) return fail(tr("The XML must use UTF-8 without null bytes."));
        QXmlStreamReader xml(data);
        xml.setEntityExpansionLimit(256);
        Event event;
        int depth = 0;
        int elements = 0;
        bool root = false;
        bool point = false;
        bool detail = false;
        bool inDetail = false;
        bool contact = false;
        const auto timestamp = [](const QString &value) {
            static const QRegularExpression zone(QStringLiteral("(?:Z|[+-][0-9]{2}:[0-9]{2})\\z"));
            if (!zone.match(value).hasMatch()) return QDateTime{};
            return QDateTime::fromString(value, Qt::ISODateWithMs).toUTC();
        };
        while (!xml.atEnd()) {
            const auto token = xml.readNext();
            if (token == QXmlStreamReader::StartDocument && !xml.documentEncoding().isEmpty() &&
                xml.documentEncoding().compare(QLatin1String("UTF-8"), Qt::CaseInsensitive) != 0)
                return fail(tr("The XML must use UTF-8."));
            if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference ||
                token == QXmlStreamReader::ProcessingInstruction)
                return fail(tr("The XML contains a prohibited declaration or reference."));
            if (token == QXmlStreamReader::StartElement) {
                if (++depth > 12 || ++elements > 256 || xml.attributes().size() > 32)
                    return fail(tr("The XML structure exceeds the supported limits."));
                const auto attrs = xml.attributes();
                const auto field = [&attrs](const char *key) { return attrs.value(QLatin1String(key)).toString(); };
                if (depth == 1) {
                    if (root || xml.name() != QLatin1String("event") || !xml.namespaceUri().isEmpty() || field("version") != QLatin1String("2.0"))
                        return fail(tr("The XML must contain one CoT 2.0 event."));
                    root = true;
                    event.uid = field("uid");
                    event.type = field("type");
                    event.how = field("how");
                    event.time = timestamp(field("time"));
                    event.start = timestamp(field("start"));
                    event.stale = timestamp(field("stale"));
                } else if (depth == 2 && xml.name() == QLatin1String("point") && xml.namespaceUri().isEmpty()) {
                    if (point) return fail(tr("The event contains more than one point."));
                    point = true;
                    const auto number = [&field](const char *key, double &out) {
                        const QString text = field(key);
                        static const QRegularExpression decimal(QStringLiteral("\\A[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)\\z"));
                        bool ok = false;
                        out = text.toDouble(&ok);
                        return ok && text.size() <= 40 && decimal.match(text).hasMatch() && std::isfinite(out);
                    };
                    if (!number("lat", event.latitude) || !number("lon", event.longitude) ||
                        !number("hae", event.altitude) || !number("ce", event.circularError) || !number("le", event.linearError))
                        return fail(tr("The point must contain five finite decimal coordinates and uncertainty values."));
                } else if (depth == 2 && xml.name() == QLatin1String("detail") && xml.namespaceUri().isEmpty()) {
                    if (detail) return fail(tr("The event contains more than one detail element."));
                    detail = true;
                    inDetail = true;
                } else if (!inDetail) {
                    return fail(tr("The event contains an unsupported element outside its detail."));
                } else if (depth == 3 && xml.name() == QLatin1String("contact") && xml.namespaceUri().isEmpty()) {
                    if (contact) return fail(tr("The event contains more than one contact."));
                    contact = true;
                    event.callsign = field("callsign");
                }
            } else if (token == QXmlStreamReader::EndElement) {
                if (depth == 2) inDetail = false;
                --depth;
            } else if (token == QXmlStreamReader::Characters && !xml.isWhitespace() && !inDetail) {
                return fail(tr("The event contains unexpected text."));
            }
        }
        if (xml.hasError() || !root || !point || depth != 0) return fail(tr("The XML is incomplete or malformed."));
        const auto error = eventError(event);
        if (!error.isEmpty()) return fail(error);
        return {event, {}};
    }
    QByteArray encode(const Event &event) const override
    {
        if (!eventError(event).isEmpty()) return {};
        QByteArray output;
        QXmlStreamWriter xml(&output);
        xml.writeStartDocument();
        xml.writeStartElement(QStringLiteral("event"));
        const auto attr = [&xml](const char *key, const QString &value) { xml.writeAttribute(QLatin1String(key), value); };
        attr("version", QStringLiteral("2.0"));
        attr("uid", event.uid);
        attr("type", event.type);
        attr("how", event.how);
        attr("time", event.time.toUTC().toString(Qt::ISODateWithMs));
        attr("start", event.start.toUTC().toString(Qt::ISODateWithMs));
        attr("stale", event.stale.toUTC().toString(Qt::ISODateWithMs));
        xml.writeStartElement(QStringLiteral("point"));
        attr("lat", QString::number(event.latitude, 'f', 8));
        attr("lon", QString::number(event.longitude, 'f', 8));
        attr("hae", QString::number(event.altitude, 'f', 3));
        attr("ce", QString::number(event.circularError, 'f', 3));
        attr("le", QString::number(event.linearError, 'f', 3));
        xml.writeEndElement();
        xml.writeStartElement(QStringLiteral("detail"));
        if (!event.callsign.isEmpty()) {
            xml.writeStartElement(QStringLiteral("contact"));
            attr("callsign", event.callsign);
            xml.writeEndElement();
        }
        xml.writeEndElement();
        xml.writeEndElement();
        xml.writeEndDocument();
        return xml.hasError() ? QByteArray{} : output;
    }
};
}
#include "cot.moc"

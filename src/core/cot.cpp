#include "core/cot.h"

#include "core/cot_detail.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cmath>
#include <optional>
namespace swak {
namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("CotPlugin", text);
}
} // namespace
DecodeResult decodeCot(const QByteArray &data)
{
    const auto fail = [](const QString &error) {
        return DecodeResult{std::nullopt, error};
    };
    if (data.isEmpty() || data.size() > MaxMessageBytes) {
        return fail(tr("The XML message must contain 1 to 65536 bytes."));
    }
    if (data.contains('\0')) {
        return fail(tr("The XML must use UTF-8 without null bytes."));
    }
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
    bool group = false;
    bool track = false;
    // The decoder copies every detail child into canonical text. Namespaced elements and their
    // children are left out, because a copy without the namespace declaration is not valid XML.
    QString detailCopy;
    QXmlStreamWriter detailWriter(&detailCopy);
    int skipFrom = 0;
    const auto timestamp = [](const QString &value) {
        static const QRegularExpression zone(QStringLiteral("(?:Z|[+-][0-9]{2}:[0-9]{2})\\z"));
        if (!zone.match(value).hasMatch()) {
            return QDateTime{};
        }
        return QDateTime::fromString(value, Qt::ISODateWithMs).toUTC();
    };
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartDocument && !xml.documentEncoding().isEmpty() &&
            xml.documentEncoding().compare(QLatin1String("UTF-8"), Qt::CaseInsensitive) != 0) {
            return fail(tr("The XML must use UTF-8."));
        }
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference ||
            token == QXmlStreamReader::ProcessingInstruction) {
            return fail(tr("The XML contains a prohibited declaration or reference."));
        }
        if (token == QXmlStreamReader::StartElement) {
            if (++depth > 12 || ++elements > 2048 ||
                xml.attributes().size() + xml.namespaceDeclarations().size() > 32) {
                return fail(tr("The XML structure exceeds the supported limits."));
            }
            const auto attrs = xml.attributes();
            const auto field = [&attrs](const char *key) {
                return attrs.value(QLatin1String(key)).toString();
            };
            if (depth == 1) {
                if (root || xml.name() != QLatin1String("event") || !xml.namespaceUri().isEmpty() ||
                    field("version") != QLatin1String("2.0")) {
                    return fail(tr("The XML must contain one CoT 2.0 event."));
                }
                root = true;
                event.uid = field("uid");
                event.type = field("type");
                event.how = field("how");
                event.time = timestamp(field("time"));
                event.start = timestamp(field("start"));
                event.stale = timestamp(field("stale"));
            } else if (depth == 2 && xml.name() == QLatin1String("point") &&
                       xml.namespaceUri().isEmpty()) {
                if (point) {
                    return fail(tr("The event contains more than one point."));
                }
                point = true;
                const auto number = [&field](const char *key, double &out) {
                    const QString text = field(key);
                    static const QRegularExpression decimal(
                        QStringLiteral("\\A[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)\\z"));
                    bool ok = false;
                    out = text.toDouble(&ok);
                    return ok && text.size() <= 40 && decimal.match(text).hasMatch() &&
                           std::isfinite(out);
                };
                if (!number("lat", event.latitude) || !number("lon", event.longitude) ||
                    !number("hae", event.altitude) || !number("ce", event.circularError) ||
                    !number("le", event.linearError)) {
                    return fail(tr("The point must contain five finite decimal coordinates and "
                                   "uncertainty values."));
                }
            } else if (depth == 2 && xml.name() == QLatin1String("detail") &&
                       xml.namespaceUri().isEmpty()) {
                if (detail) {
                    return fail(tr("The event contains more than one detail element."));
                }
                detail = true;
                inDetail = true;
            } else if (!inDetail) {
                return fail(tr("The event contains an unsupported element outside its detail."));
            }
            if (inDetail && depth >= 3) {
                if (!skipFrom && !xml.namespaceUri().isEmpty()) {
                    skipFrom = depth;
                }
                if (!skipFrom) {
                    detailWriter.writeStartElement(xml.name().toString());
                    for (const auto &attribute : attrs) {
                        if (attribute.namespaceUri().isEmpty()) {
                            detailWriter.writeAttribute(attribute.name().toString(),
                                                        attribute.value().toString());
                        }
                    }
                }
            }
            if (!inDetail || depth != 3) {
                continue;
            }
            if (xml.name() == QLatin1String("contact") && xml.namespaceUri().isEmpty()) {
                if (contact) {
                    return fail(tr("The event contains more than one contact."));
                }
                contact = true;
                event.callsign = field("callsign");
            } else if (xml.name() == QLatin1String("track") && xml.namespaceUri().isEmpty()) {
                if (track) {
                    return fail(tr("The event contains more than one track."));
                }
                track = true;
                for (const auto *key : {"course", "speed"}) {
                    if (!attrs.hasAttribute(QLatin1String(key))) {
                        continue;
                    }
                    const auto text = field(key);
                    static const QRegularExpression decimal(
                        QStringLiteral("\\A[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)\\z"));
                    bool ok = false;
                    const double value = text.toDouble(&ok);
                    if (!ok || text.size() > 40 || !decimal.match(text).hasMatch() ||
                        !std::isfinite(value)) {
                        return fail(tr("Track heading and speed must be finite decimal numbers."));
                    }
                    if (QLatin1String(key) == QLatin1String("course")) {
                        event.course = value;
                    } else {
                        event.speed = value;
                    }
                }
            } else if (xml.name() == QLatin1String("__group") && xml.namespaceUri().isEmpty()) {
                if (group) {
                    return fail(tr("The event contains more than one group."));
                }
                group = true;
                event.team = field("name");
                event.role = field("role");
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (depth == 2) {
                inDetail = false;
            }
            if (inDetail && depth >= 3) {
                if (!skipFrom) {
                    detailWriter.writeEndElement();
                } else if (skipFrom == depth) {
                    skipFrom = 0;
                }
            }
            --depth;
        } else if (token == QXmlStreamReader::Characters && !xml.isWhitespace()) {
            if (!inDetail) {
                return fail(tr("The event contains unexpected text."));
            }
            if (depth >= 3 && !skipFrom) {
                detailWriter.writeCharacters(xml.text().toString());
            }
        }
    }
    if (xml.hasError() || !root || !point || depth != 0) {
        return fail(tr("The XML is incomplete or malformed."));
    }
    if (detailCopy.size() > MaxDetailCharacters || detailWriter.hasError()) {
        event.detailTruncated = true;
    } else {
        event.detail = detailCopy;
    }
    const auto error = eventError(event);
    if (!error.isEmpty()) {
        return fail(error);
    }
    return {event, {}};
}
std::vector<CotElement> mergedDetail(const Event &event)
{
    // Typed fields replace their attributes in the retained elements. Other attributes,
    // children, and elements stay as received, so a forwarded event keeps its detail.
    auto elements = detailElements(event.detail);
    const auto typed = [&elements](const QString &name) -> CotElement & {
        const auto found = std::find_if(elements.begin(), elements.end(), [&name](const auto &e) {
            return e.name == name;
        });
        if (found != elements.end()) {
            return *found;
        }
        elements.insert(elements.begin(), CotElement{name, {}, {}, {}});
        return elements.front();
    };
    const auto remove = [&elements](const QString &name) {
        elements.erase(std::remove_if(elements.begin(),
                                      elements.end(),
                                      [&name](const auto &e) {
                                          return e.name == name;
                                      }),
                       elements.end());
    };
    if (event.team.isEmpty() && event.role.isEmpty()) {
        remove(QStringLiteral("__group"));
    } else {
        auto &group = typed(QStringLiteral("__group"));
        group.setAttribute(QStringLiteral("name"), event.team);
        group.setAttribute(QStringLiteral("role"), event.role);
    }
    if (!event.course && !event.speed) {
        remove(QStringLiteral("track"));
    } else {
        auto &track = typed(QStringLiteral("track"));
        track.attributes.removeIf([](const auto &pair) {
            return pair.first == u"course" || pair.first == u"speed";
        });
        if (event.course) {
            track.setAttribute(QStringLiteral("course"), QString::number(*event.course, 'f', 6));
        }
        if (event.speed) {
            track.setAttribute(QStringLiteral("speed"), QString::number(*event.speed, 'f', 6));
        }
    }
    if (!event.callsign.isEmpty()) {
        typed(QStringLiteral("contact")).setAttribute(QStringLiteral("callsign"), event.callsign);
    } else {
        for (auto &element : elements) {
            if (element.name == u"contact") {
                element.attributes.removeIf([](const auto &pair) {
                    return pair.first == u"callsign";
                });
            }
        }
    }
    return elements;
}
QByteArray encodeCot(const Event &event)
{
    if (!eventError(event).isEmpty()) {
        return {};
    }
    QByteArray output;
    QXmlStreamWriter xml(&output);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("event"));
    const auto attr = [&xml](const char *key, const QString &value) {
        xml.writeAttribute(QLatin1String(key), value);
    };
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
    if (!writeDetail(xml, mergedDetail(event))) {
        return {};
    }
    xml.writeEndElement();
    xml.writeEndElement();
    xml.writeEndDocument();
    return xml.hasError() ? QByteArray{} : output;
}
} // namespace swak

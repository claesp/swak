#include "core/cot_detail.h"

#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace swak {
namespace {
constexpr int MaxDetailDepth = 10;
constexpr int MaxDetailElements = 2048;
constexpr int MaxReadElements = 64;
// ATAK routes and shapes carry one link for each point.
constexpr int MaxReadLinks = 1024;

bool validName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z_][A-Za-z0-9._-]{0,63}\\z"));
    return pattern.match(name).hasMatch();
}
bool safeValue(const QString &value)
{
    return std::none_of(value.begin(), value.end(), [](QChar ch) {
        return ch.isNull() || ch.isSurrogate() ||
               (ch.category() == QChar::Other_Control && ch != u'\n' && ch != u'\t');
    });
}
void writeElement(QXmlStreamWriter &xml, const CotElement &element, int depth, bool &valid)
{
    if (depth > MaxDetailDepth || !validName(element.name)) {
        valid = false;
        return;
    }
    xml.writeStartElement(element.name);
    for (const auto &[key, value] : element.attributes) {
        if (!validName(key) || !safeValue(value)) {
            valid = false;
            continue;
        }
        xml.writeAttribute(key, value);
    }
    if (!element.text.isEmpty() && safeValue(element.text)) {
        xml.writeCharacters(element.text);
    }
    for (const auto &child : element.children) {
        writeElement(xml, child, depth + 1, valid);
    }
    xml.writeEndElement();
}
QString attributeOf(const std::optional<CotElement> &element, QStringView key)
{
    return element ? element->attribute(key) : QString{};
}
} // namespace

QString CotElement::attribute(QStringView key) const
{
    const auto found =
        std::find_if(attributes.cbegin(), attributes.cend(), [key](const auto &pair) {
            return pair.first == key;
        });
    return found == attributes.cend() ? QString{} : found->second;
}
const CotElement *CotElement::child(QStringView childName) const
{
    const auto found =
        std::find_if(children.cbegin(), children.cend(), [childName](const auto &item) {
            return item.name == childName;
        });
    return found == children.cend() ? nullptr : &*found;
}
std::vector<CotElement> detailElements(const QString &detail)
{
    if (detail.isEmpty() || detail.size() > MaxDetailCharacters) {
        return {};
    }
    // The decoder writes this text. Parse it again with the same limits for safety.
    QXmlStreamReader xml(QStringLiteral("<detail>") + detail + QStringLiteral("</detail>"));
    std::vector<CotElement> root;
    std::vector<CotElement *> stack;
    int elements = 0;
    bool wrapped = false;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference ||
            token == QXmlStreamReader::ProcessingInstruction) {
            return {};
        }
        if (token == QXmlStreamReader::StartElement) {
            // The first element is the wrapper above.
            if (!wrapped) {
                wrapped = true;
                continue;
            }
            if (++elements > MaxDetailElements ||
                static_cast<int>(stack.size()) >= MaxDetailDepth) {
                return {};
            }
            CotElement element;
            element.name = xml.name().toString();
            for (const auto &attribute : xml.attributes()) {
                element.attributes.append(
                    {attribute.name().toString(), attribute.value().toString()});
            }
            auto &siblings = stack.empty() ? root : stack.back()->children;
            siblings.push_back(std::move(element));
            stack.push_back(&siblings.back());
        } else if (token == QXmlStreamReader::EndElement) {
            if (!stack.empty()) {
                stack.pop_back();
            }
        } else if (token == QXmlStreamReader::Characters && !stack.empty() && !xml.isWhitespace()) {
            stack.back()->text += xml.text();
        }
    }
    return xml.hasError() ? std::vector<CotElement>{} : root;
}
void writeDetail(QXmlStreamWriter &xml, const std::vector<CotElement> &elements)
{
    bool valid = true;
    for (const auto &element : elements) {
        writeElement(xml, element, 1, valid);
    }
}
// Later TAK phases use this reader. The server tests cover it now.
// cppcheck-suppress unusedFunction
QString detailText(const std::vector<CotElement> &elements)
{
    QString output;
    QXmlStreamWriter xml(&output);
    writeDetail(xml, elements);
    if (xml.hasError() || output.size() > MaxDetailCharacters) {
        return {};
    }
    return output;
}
void CotElement::setAttribute(const QString &key, const QString &value)
{
    const auto found = std::find_if(attributes.begin(), attributes.end(), [&key](const auto &pair) {
        return pair.first == key;
    });
    if (found == attributes.end()) {
        attributes.append({key, value});
    } else {
        found->second = value;
    }
}
std::optional<CotElement> detailElement(const Event &event, QStringView name)
{
    auto elements = detailElements(event.detail);
    const auto found = std::find_if(elements.begin(), elements.end(), [name](const auto &item) {
        return item.name == name;
    });
    if (found == elements.end()) {
        return {};
    }
    return std::move(*found);
}
QString remarks(const Event &event)
{
    const auto element = detailElement(event, u"remarks");
    return element ? element->text.left(MaxDetailCharacters) : QString{};
}
QString contactEndpoint(const Event &event)
{
    return attributeOf(detailElement(event, u"contact"), u"endpoint").left(256);
}
QList<CotLink> links(const Event &event)
{
    QList<CotLink> result;
    for (const auto &element : detailElements(event.detail)) {
        if (element.name != u"link" || result.size() >= MaxReadLinks) {
            continue;
        }
        const auto callsign = element.attribute(u"parent_callsign");
        result.append({element.attribute(u"uid"),
                       element.attribute(u"type"),
                       element.attribute(u"relation"),
                       element.attribute(u"point"),
                       callsign.isEmpty() ? element.attribute(u"callsign") : callsign,
                       element.attribute(u"url"),
                       element.attribute(u"mime")});
    }
    return result;
}
// Later TAK phases use this reader. The server tests cover it now.
// cppcheck-suppress unusedFunction
QList<CotDestination> destinations(const Event &event)
{
    QList<CotDestination> result;
    const auto marti = detailElement(event, u"marti");
    if (!marti) {
        return result;
    }
    for (const auto &dest : marti->children) {
        if (dest.name != u"dest" || result.size() >= MaxReadElements) {
            continue;
        }
        result.append({dest.attribute(u"callsign"),
                       dest.attribute(u"uid"),
                       dest.attribute(u"mission"),
                       dest.attribute(u"group")});
    }
    return result;
}
// Later TAK phases use this reader. The server tests cover it now.
// cppcheck-suppress unusedFunction
std::optional<CotEmergency> emergency(const Event &event)
{
    const auto element = detailElement(event, u"emergency");
    if (!element) {
        return {};
    }
    return CotEmergency{element->attribute(u"type").left(128),
                        element->text.left(256),
                        element->attribute(u"cancel").compare(u"true", Qt::CaseInsensitive) == 0};
}
// Later TAK phases use this reader. The server tests cover it now.
// cppcheck-suppress unusedFunction
std::optional<CotFileShare> fileShare(const Event &event)
{
    const auto element = detailElement(event, u"fileshare");
    if (!element) {
        return {};
    }
    CotFileShare result{element->attribute(u"filename"),
                        element->attribute(u"name"),
                        element->attribute(u"senderUrl"),
                        element->attribute(u"senderUid"),
                        element->attribute(u"senderCallsign"),
                        element->attribute(u"sha256").toLower(),
                        -1};
    bool number = false;
    const auto size = element->attribute(u"sizeInBytes").toLongLong(&number);
    result.sizeInBytes = number && size >= 0 ? size : -1;
    static const QRegularExpression hash(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    if (!hash.match(result.sha256).hasMatch()) {
        result.sha256.clear();
    }
    return result;
}
// Later TAK phases use this reader. The server tests cover it now.
// cppcheck-suppress unusedFunction
std::optional<CotChat> chat(const Event &event)
{
    const auto element = detailElement(event, u"__chat");
    if (!element) {
        return {};
    }
    return CotChat{element->attribute(u"messageId"),
                   element->attribute(u"chatroom"),
                   element->attribute(u"id"),
                   element->attribute(u"senderCallsign"),
                   element->attribute(u"parent"),
                   element->attribute(u"groupOwner").compare(u"true", Qt::CaseInsensitive) == 0};
}
// Later TAK phases use this reader. The server tests cover it now.
// cppcheck-suppress unusedFunction
QString videoUrl(const Event &event)
{
    const auto element = detailElement(event, u"__video");
    if (!element) {
        return {};
    }
    if (const auto url = element->attribute(u"url"); !url.isEmpty()) {
        return url.left(2048);
    }
    const auto *entry = element->child(u"ConnectionEntry");
    if (!entry || entry->attribute(u"address").isEmpty()) {
        return {};
    }
    auto url = entry->attribute(u"protocol") + QStringLiteral("://") + entry->attribute(u"address");
    if (const auto port = entry->attribute(u"port"); !port.isEmpty() && port != u"-1") {
        url += QLatin1Char(':') + port;
    }
    const auto path = entry->attribute(u"path");
    if (!path.isEmpty()) {
        url += path.startsWith(QLatin1Char('/')) ? path : QLatin1Char('/') + path;
    }
    return url.left(2048);
}
// Later TAK phases use this reader. The server tests cover it now.
// cppcheck-suppress unusedFunction
QString iconsetPath(const Event &event)
{
    return attributeOf(detailElement(event, u"usericon"), u"iconsetpath").left(512);
}
// Later TAK phases use this reader. The server tests cover it now.
// cppcheck-suppress unusedFunction
std::optional<QColor> detailColor(const Event &event, QStringView name)
{
    const auto element = detailElement(event, name);
    if (!element) {
        return {};
    }
    auto text = element->attribute(u"argb");
    if (text.isEmpty()) {
        text = element->attribute(u"value");
    }
    bool number = false;
    const auto value = text.toLongLong(&number);
    if (!number || value < std::numeric_limits<qint32>::min() ||
        value > std::numeric_limits<quint32>::max()) {
        return {};
    }
    return QColor::fromRgba(static_cast<QRgb>(static_cast<quint32>(value)));
}
std::optional<int> batteryPercent(const Event &event)
{
    bool number = false;
    const int value = attributeOf(detailElement(event, u"status"), u"battery").toInt(&number);
    if (!number || value < 0 || value > 100) {
        return {};
    }
    return value;
}
} // namespace swak

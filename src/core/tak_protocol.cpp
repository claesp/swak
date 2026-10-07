#include "core/tak_protocol.h"

#include "core/cot.h"
#include "core/cot_detail.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <tak_protocol.pb.h>

namespace swak {
namespace {
// The latest time that ISO 8601 text with four year digits can show.
constexpr quint64 MaxTimeMs = 253402300799999ULL;
constexpr int MaxVarintBytes = 5;
const QString OtherDetail = QStringLiteral("SWAK-OTHER-DETAIL");

QString tr(const char *text)
{
    return QCoreApplication::translate("TakProtocol", text);
}
std::string utf8(const QString &text)
{
    return text.toStdString();
}
QString text(const std::string &value)
{
    return QString::fromStdString(value);
}
QString isoTime(quint64 milliseconds)
{
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(std::min(milliseconds, MaxTimeMs)),
                                          QTimeZone::UTC)
        .toString(Qt::ISODateWithMs);
}
quint64 milliseconds(const QDateTime &time)
{
    return static_cast<quint64>(std::max<qint64>(0, time.toMSecsSinceEpoch()));
}
// True when the element holds only these attributes, without text or children.
bool onlyAttributes(const CotElement &element, std::initializer_list<QStringView> keys)
{
    return element.children.empty() && element.text.trimmed().isEmpty() &&
           std::all_of(
               element.attributes.cbegin(), element.attributes.cend(), [&keys](const auto &pair) {
                   return std::find(keys.begin(), keys.end(), QStringView(pair.first)) !=
                          keys.end();
               });
}
std::optional<double> number(const QString &value)
{
    bool ok = false;
    const double result = value.toDouble(&ok);
    return ok && std::isfinite(result) ? std::optional<double>(result) : std::nullopt;
}
void appendVarint(QByteArray &out, quint64 value)
{
    do {
        auto byte = static_cast<char>(value & 0x7f);
        value >>= 7;
        if (value) {
            byte = static_cast<char>(byte | 0x80);
        }
        out.append(byte);
    } while (value);
}
// Reads a varint at offset. Returns the number of bytes read, 0 when more bytes are needed,
// or -1 for an invalid varint.
int readVarint(QByteArrayView bytes, qsizetype offset, quint64 &value)
{
    value = 0;
    for (int i = 0; i < MaxVarintBytes; ++i) {
        if (offset + i >= bytes.size()) {
            return 0;
        }
        const auto byte = static_cast<quint8>(bytes.at(offset + i));
        value |= static_cast<quint64>(byte & 0x7f) << (7 * i);
        if (!(byte & 0x80)) {
            return i + 1;
        }
    }
    return -1;
}
} // namespace

QByteArray encodeTakMessage(const Event &event)
{
    if (!eventError(event).isEmpty()) {
        return {};
    }
    tak::v1::TakMessage message;
    auto *cot = message.mutable_event();
    cot->set_type(utf8(event.type));
    cot->set_uid(utf8(event.uid));
    cot->set_how(utf8(event.how));
    cot->set_time_ms(milliseconds(event.time));
    cot->set_start_ms(milliseconds(event.start));
    cot->set_stale_ms(milliseconds(event.stale));
    cot->set_latitude(event.latitude);
    cot->set_longitude(event.longitude);
    cot->set_height_above_ellipsoid(event.altitude);
    cot->set_circular_error(event.circularError);
    cot->set_linear_error(event.linearError);
    auto *detail = cot->mutable_detail();
    std::vector<CotElement> others;
    for (auto &element : mergedDetail(event)) {
        const auto &name = element.name;
        if (name == u"contact" && !detail->has_contact() &&
            onlyAttributes(element, {u"endpoint", u"callsign"})) {
            auto *contact = detail->mutable_contact();
            contact->set_endpoint(utf8(element.attribute(u"endpoint")));
            contact->set_callsign(utf8(element.attribute(u"callsign")));
        } else if (name == u"__group" && !detail->has_group() &&
                   onlyAttributes(element, {u"name", u"role"})) {
            auto *group = detail->mutable_group();
            group->set_team(utf8(element.attribute(u"name")));
            group->set_role(utf8(element.attribute(u"role")));
        } else if (name == u"precisionlocation" && !detail->has_precision_location() &&
                   onlyAttributes(element, {u"geopointsrc", u"altsrc"})) {
            auto *precision = detail->mutable_precision_location();
            precision->set_position_source(utf8(element.attribute(u"geopointsrc")));
            precision->set_altitude_source(utf8(element.attribute(u"altsrc")));
        } else if (name == u"takv" && !detail->has_device_version() &&
                   onlyAttributes(element, {u"device", u"platform", u"os", u"version"})) {
            auto *device = detail->mutable_device_version();
            device->set_device(utf8(element.attribute(u"device")));
            device->set_platform(utf8(element.attribute(u"platform")));
            device->set_operating_system(utf8(element.attribute(u"os")));
            device->set_version(utf8(element.attribute(u"version")));
        } else if (const auto battery = number(element.attribute(u"battery"));
                   name == u"status" && !detail->has_status() &&
                   onlyAttributes(element, {u"battery"}) && battery && *battery >= 0 &&
                   *battery <= 100 && std::floor(*battery) == *battery) {
            detail->mutable_status()->set_battery_percent(static_cast<quint32>(*battery));
        } else if (const auto speed = number(element.attribute(u"speed")),
                   course = number(element.attribute(u"course"));
                   name == u"track" && !detail->has_track() &&
                   onlyAttributes(element, {u"speed", u"course"}) && speed && course) {
            // The typed track needs both values. A partial track stays XML, so that a missing
            // speed does not become zero.
            detail->mutable_track()->set_speed(*speed);
            detail->mutable_track()->set_course(*course);
        } else {
            others.push_back(std::move(element));
        }
    }
    if (!others.empty()) {
        detail->set_other_xml(utf8(detailText(others)));
    }
    std::string output;
    if (!message.SerializeToString(&output) ||
        output.size() > static_cast<size_t>(MaxMessageBytes)) {
        return {};
    }
    return QByteArray::fromStdString(output);
}
TakDecodeResult decodeTakMessage(QByteArrayView payload)
{
    TakDecodeResult result;
    tak::v1::TakMessage message;
    if (payload.isEmpty() || payload.size() > MaxMessageBytes ||
        !message.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        result.error = tr("The binary TAK message is malformed.");
        return result;
    }
    if (!message.has_event()) {
        result.control = true;
        return result;
    }
    const auto &cot = message.event();
    // Rebuild CoT XML. The writer escapes every value. The other detail XML goes in at the
    // marker below, and decodeCot then checks the whole document.
    QString xml;
    QXmlStreamWriter writer(&xml);
    writer.writeStartElement(QStringLiteral("event"));
    writer.writeAttribute(QStringLiteral("version"), QStringLiteral("2.0"));
    writer.writeAttribute(QStringLiteral("uid"), text(cot.uid()));
    writer.writeAttribute(QStringLiteral("type"), text(cot.type()));
    writer.writeAttribute(QStringLiteral("how"), text(cot.how()));
    writer.writeAttribute(QStringLiteral("time"), isoTime(cot.time_ms()));
    writer.writeAttribute(QStringLiteral("start"), isoTime(cot.start_ms()));
    writer.writeAttribute(QStringLiteral("stale"), isoTime(cot.stale_ms()));
    writer.writeStartElement(QStringLiteral("point"));
    writer.writeAttribute(QStringLiteral("lat"), QString::number(cot.latitude(), 'f', 8));
    writer.writeAttribute(QStringLiteral("lon"), QString::number(cot.longitude(), 'f', 8));
    writer.writeAttribute(QStringLiteral("hae"),
                          QString::number(cot.height_above_ellipsoid(), 'f', 3));
    writer.writeAttribute(QStringLiteral("ce"), QString::number(cot.circular_error(), 'f', 3));
    writer.writeAttribute(QStringLiteral("le"), QString::number(cot.linear_error(), 'f', 3));
    writer.writeEndElement();
    writer.writeStartElement(QStringLiteral("detail"));
    const auto element = [&writer](const QString &name,
                                   std::initializer_list<std::pair<const char *, QString>> values) {
        writer.writeStartElement(name);
        for (const auto &[key, value] : values) {
            if (!value.isEmpty()) {
                writer.writeAttribute(QLatin1String(key), value);
            }
        }
        writer.writeEndElement();
    };
    const auto &detail = cot.detail();
    if (detail.has_contact()) {
        element(QStringLiteral("contact"),
                {{"endpoint", text(detail.contact().endpoint())},
                 {"callsign", text(detail.contact().callsign())}});
    }
    if (detail.has_group()) {
        element(QStringLiteral("__group"),
                {{"name", text(detail.group().team())}, {"role", text(detail.group().role())}});
    }
    if (detail.has_precision_location()) {
        element(QStringLiteral("precisionlocation"),
                {{"geopointsrc", text(detail.precision_location().position_source())},
                 {"altsrc", text(detail.precision_location().altitude_source())}});
    }
    if (detail.has_status()) {
        element(QStringLiteral("status"),
                {{"battery", QString::number(detail.status().battery_percent())}});
    }
    if (detail.has_device_version()) {
        const auto &device = detail.device_version();
        element(QStringLiteral("takv"),
                {{"device", text(device.device())},
                 {"platform", text(device.platform())},
                 {"os", text(device.operating_system())},
                 {"version", text(device.version())}});
    }
    if (detail.has_track()) {
        element(QStringLiteral("track"),
                {{"speed", QString::number(detail.track().speed(), 'f', 6)},
                 {"course", QString::number(detail.track().course(), 'f', 6)}});
    }
    writer.writeComment(OtherDetail);
    writer.writeEndElement();
    writer.writeEndElement();
    xml.replace(QStringLiteral("<!--") + OtherDetail + QStringLiteral("-->"),
                text(detail.other_xml()));
    const auto bytes = xml.toUtf8();
    if (bytes.size() > MaxMessageBytes) {
        result.error = tr("The binary TAK message is too large.");
        return result;
    }
    auto decoded = decodeCot(bytes);
    result.event = std::move(decoded.event);
    result.error = decoded.error;
    return result;
}
QByteArray takStreamFrame(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MaxMessageBytes) {
        return {};
    }
    QByteArray frame(1, TakMagic);
    appendVarint(frame, static_cast<quint64>(payload.size()));
    return frame + payload;
}
// SWAK sends XML on the mesh. Tests build binary datagrams with this function.
// cppcheck-suppress unusedFunction
QByteArray takMeshDatagram(const QByteArray &payload)
{
    if (payload.isEmpty() || payload.size() > MaxMessageBytes) {
        return {};
    }
    QByteArray datagram(1, TakMagic);
    appendVarint(datagram, 1);
    datagram.append(TakMagic);
    return datagram + payload;
}
std::optional<QByteArray> takMeshPayload(QByteArrayView datagram)
{
    quint64 version = 0;
    if (datagram.size() < 4 || datagram.at(0) != TakMagic) {
        return {};
    }
    const int read = readVarint(datagram, 1, version);
    if (read <= 0 || version != 1 || datagram.size() <= 1 + read + 1 ||
        datagram.at(1 + read) != TakMagic) {
        return {};
    }
    const auto payload = datagram.sliced(2 + read);
    if (payload.size() > MaxMessageBytes) {
        return {};
    }
    return payload.toByteArray();
}
QList<QByteArray> TakStream::feed(const QByteArray &bytes)
{
    QList<QByteArray> payloads;
    if (failed_) {
        return payloads;
    }
    buffer_.append(bytes);
    while (!buffer_.isEmpty()) {
        if (buffer_.at(0) != TakMagic) {
            failed_ = true;
            break;
        }
        quint64 length = 0;
        const int read = readVarint(buffer_, 1, length);
        if (read < 0 ||
            (read > 0 && (length == 0 || length > static_cast<quint64>(MaxMessageBytes)))) {
            failed_ = true;
            break;
        }
        if (read == 0 || buffer_.size() < 1 + read + static_cast<qsizetype>(length)) {
            break;
        }
        payloads.append(buffer_.mid(1 + read, static_cast<qsizetype>(length)));
        buffer_.remove(0, 1 + read + static_cast<qsizetype>(length));
    }
    // A waiting frame never holds more than one maximum frame.
    if (failed_ || buffer_.size() > MaxMessageBytes + 1 + MaxVarintBytes) {
        failed_ = true;
        buffer_.clear();
    }
    return payloads;
}
} // namespace swak

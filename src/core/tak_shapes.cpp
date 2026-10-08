#include "core/tak_shapes.h"

#include "core/cot_detail.h"
#include "core/marker_collections.h"
#include "core/marker_symbols.h"
#include "core/measure.h"
#include "core/polygon.h"
#include "core/projection.h"

#include <QRectF>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace swak {
namespace {
// TAK draws filled areas with this alpha, so the map stays visible below them.
constexpr int FillAlpha = 0x60;
constexpr double UnknownHeight = 9999999;

QString pointText(QPointF world)
{
    const auto position = unproject(world);
    return QStringLiteral("%1,%2").arg(QString::number(position.y(), 'f', 7),
                                       QString::number(position.x(), 'f', 7));
}
CotElement element(const QString &name, QList<std::pair<QString, QString>> attributes = {})
{
    return {name, std::move(attributes), {}, {}};
}
CotElement colorElement(const QString &name, const QColor &color)
{
    return element(name, {{QStringLiteral("value"), takArgb(color)}});
}
// Adds the marti element that directs delivery. TAK Server matches dest callsigns like ATAK.
void appendRecipients(std::vector<CotElement> &elements, const QList<ShareRecipient> &recipients)
{
    if (recipients.isEmpty()) {
        return;
    }
    auto marti = element(QStringLiteral("marti"));
    for (const auto &recipient : recipients) {
        marti.children.push_back(
            recipient.callsign.isEmpty()
                ? element(QStringLiteral("dest"), {{QStringLiteral("uid"), recipient.uid}})
                : element(QStringLiteral("dest"),
                          {{QStringLiteral("callsign"), recipient.callsign}}));
    }
    elements.push_back(std::move(marti));
}
// The common elements of every shared object: the sender link, the text, and the recipients.
std::vector<CotElement>
commonDetail(const UserObject &item, const Event &sender, const QList<ShareRecipient> &recipients)
{
    std::vector<CotElement> elements;
    elements.push_back(element(QStringLiteral("link"),
                               {{QStringLiteral("uid"), sender.uid},
                                {QStringLiteral("type"), sender.type},
                                {QStringLiteral("parent_callsign"), sender.callsign},
                                {QStringLiteral("relation"), QStringLiteral("p-p")}}));
    elements.push_back(element(QStringLiteral("archive")));
    if (!item.description.isEmpty()) {
        auto text = element(QStringLiteral("remarks"));
        text.text = item.description;
        elements.push_back(std::move(text));
    }
    appendRecipients(elements, recipients);
    return elements;
}
void setPosition(Event &event, QPointF world)
{
    const auto position = unproject(world);
    event.latitude = position.y();
    event.longitude = position.x();
}
// True when a marker uses a symbol of the military collection. Its identifier is a CoT type.
bool militarySymbol(const UserObject &item)
{
    return !item.symbol.isEmpty() &&
           effectiveMarkerCollection(item.collection, item.symbol) == MilitaryMarkerCollection &&
           markerSymbolForCot(item.symbol) == item.symbol;
}
QPointF center(const QPolygonF &points)
{
    return points.boundingRect().center();
}
} // namespace

QString takArgb(const QColor &color)
{
    // TAK stores the unsigned ARGB value as a signed 32-bit integer.
    return QString::number(static_cast<qint32>(static_cast<quint32>(color.rgba())));
}
QString sharedCotType(const UserObject &item)
{
    if (item.type == QStringLiteral("circle")) {
        // A military symbol is a CoT unit type, so TAK clients show the same symbol.
        return militarySymbol(item) ? item.symbol : QStringLiteral("b-m-p-s-m");
    }
    if (item.type == QStringLiteral("range-circle")) {
        return QStringLiteral("u-d-c-c");
    }
    if (item.type == QStringLiteral("line") || item.type == QStringLiteral("polygon")) {
        return QStringLiteral("u-d-f");
    }
    if (item.type == QStringLiteral("route")) {
        return QStringLiteral("b-m-r");
    }
    return {};
}
std::optional<Event> sharedObjectEvent(const UserObject &item,
                                       const Event &sender,
                                       const QList<ShareRecipient> &recipients,
                                       const QDateTime &now)
{
    const auto type = sharedCotType(item);
    if (type.isEmpty() || !UserObjects::validate(item).isEmpty()) {
        return {};
    }
    Event event;
    event.uid = item.id;
    event.type = type;
    event.how = QStringLiteral("h-e");
    event.callsign = item.title;
    event.time = event.start = now;
    event.stale = now.addSecs(SharedObjectStaleSeconds);
    event.altitude = UnknownHeight;
    auto detail = commonDetail(item, sender, recipients);
    const QColor stroke(item.color);
    auto fill = stroke;
    fill.setAlpha(FillAlpha);
    if (item.type == QStringLiteral("circle")) {
        setPosition(event,
                    {item.geometry.value(QStringLiteral("x")).toDouble(),
                     item.geometry.value(QStringLiteral("y")).toDouble()});
        if (!militarySymbol(item)) {
            detail.push_back(
                element(QStringLiteral("color"), {{QStringLiteral("argb"), takArgb(stroke)}}));
            detail.push_back(
                element(QStringLiteral("usericon"),
                        {{QStringLiteral("iconsetpath"),
                          QStringLiteral("COT_MAPPING_SPOTMAP/b-m-p-s-m/") + takArgb(stroke)}}));
        }
    } else if (item.type == QStringLiteral("range-circle")) {
        const auto points = polygonVertices(item.geometry);
        setPosition(event, points.at(0));
        const auto radius = QString::number(groundDistance(points.at(0), points.at(1)), 'f', 2);
        auto shape = element(QStringLiteral("shape"));
        shape.children.push_back(element(QStringLiteral("ellipse"),
                                         {{QStringLiteral("major"), radius},
                                          {QStringLiteral("minor"), radius},
                                          {QStringLiteral("angle"), QStringLiteral("360")}}));
        detail.push_back(std::move(shape));
        detail.push_back(colorElement(QStringLiteral("strokeColor"), stroke));
        detail.push_back(element(QStringLiteral("strokeWeight"),
                                 {{QStringLiteral("value"), QStringLiteral("3")}}));
        detail.push_back(colorElement(QStringLiteral("fillColor"), fill));
    } else if (item.type == QStringLiteral("route")) {
        const auto points = routePoints(item.geometry);
        setPosition(event, points.first());
        for (qsizetype i = 0; i < points.size(); ++i) {
            detail.push_back(
                element(QStringLiteral("link"),
                        {{QStringLiteral("uid"), item.id + QLatin1Char('.') + QString::number(i)},
                         {QStringLiteral("callsign"), QStringLiteral("CP%1").arg(i + 1)},
                         {QStringLiteral("type"), QStringLiteral("b-m-p-w")},
                         {QStringLiteral("point"), pointText(points.at(i))},
                         {QStringLiteral("relation"), QStringLiteral("c")}}));
        }
        detail.push_back(
            element(QStringLiteral("link_attr"),
                    {{QStringLiteral("color"), takArgb(stroke)},
                     {QStringLiteral("method"), QStringLiteral("Walking")},
                     {QStringLiteral("prefix"), QStringLiteral("CP")},
                     {QStringLiteral("type"), QStringLiteral("On Foot")},
                     {QStringLiteral("direction"), QStringLiteral("Infil")},
                     {QStringLiteral("routetype"), QStringLiteral("Primary")},
                     {QStringLiteral("order"), QStringLiteral("Ascending Check Points")}}));
        detail.push_back(colorElement(QStringLiteral("strokeColor"), stroke));
    } else {
        // Lines and areas are u-d-f shapes. TAK closes an area with the first point again.
        auto points = polygonVertices(item.geometry);
        setPosition(event, center(points));
        const bool area = item.type == QStringLiteral("polygon");
        if (area) {
            points.append(points.first());
        }
        std::transform(points.cbegin(), points.cend(), std::back_inserter(detail), [](QPointF p) {
            return element(QStringLiteral("link"), {{QStringLiteral("point"), pointText(p)}});
        });
        detail.push_back(colorElement(QStringLiteral("strokeColor"), stroke));
        detail.push_back(element(QStringLiteral("strokeWeight"),
                                 {{QStringLiteral("value"), QStringLiteral("3")}}));
        if (area) {
            detail.push_back(colorElement(QStringLiteral("fillColor"), fill));
        }
    }
    event.detail = detailText(detail);
    if (event.detail.isEmpty() || !eventError(event).isEmpty()) {
        return {};
    }
    return event;
}
Event sharedDeleteEvent(const QString &uid,
                        const QString &type,
                        const QList<ShareRecipient> &recipients,
                        const QDateTime &now)
{
    Event event;
    event.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    event.type = QStringLiteral("t-x-d-d");
    event.how = QStringLiteral("h-e");
    event.time = event.start = now;
    event.stale = now.addSecs(60);
    event.altitude = UnknownHeight;
    std::vector<CotElement> detail{element(QStringLiteral("link"),
                                           {{QStringLiteral("uid"), uid},
                                            {QStringLiteral("type"), type},
                                            {QStringLiteral("relation"), QStringLiteral("none")}}),
                                   element(QStringLiteral("__forcedelete"))};
    appendRecipients(detail, recipients);
    event.detail = detailText(detail);
    return event;
}
} // namespace swak

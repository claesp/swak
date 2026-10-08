#pragma once
#include "core/event.h"

#include <QColor>
#include <QList>
#include <QString>

#include <optional>
#include <utility>
#include <vector>

class QXmlStreamWriter;

namespace swak {
// Retained detail text per event. Larger detail keeps only the typed fields.
inline constexpr qsizetype MaxDetailCharacters = 32 * 1024;

// One detail element. Readers build these trees on demand from Event::detail.
struct CotElement
{
    QString name;
    QList<std::pair<QString, QString>> attributes;
    QString text;
    std::vector<CotElement> children;
    QString attribute(QStringView key) const;
    void setAttribute(const QString &key, const QString &value);
    const CotElement *child(QStringView childName) const;
};

// Parses the canonical detail text of an event. Invalid text gives an empty list.
std::vector<CotElement> detailElements(const QString &detail);
// Writes elements as canonical detail text. It skips elements with invalid names and
// returns an empty string when the result exceeds MaxDetailCharacters.
QString detailText(const std::vector<CotElement> &elements);
// Writes elements into an open XML writer. It skips elements with invalid names or values.
void writeDetail(QXmlStreamWriter &xml, const std::vector<CotElement> &elements);
// Returns the first top-level detail element with this name.
std::optional<CotElement> detailElement(const Event &event, QStringView name);

struct CotLink
{
    QString uid;
    QString type;
    QString relation;
    QString point;
    QString callsign;
    QString url;
    QString mime;
};
struct CotDestination
{
    QString callsign;
    QString uid;
    QString mission;
    QString group;
};
struct CotEmergency
{
    QString type;
    QString text;
    bool cancel = false;
};
struct CotFileShare
{
    QString fileName;
    QString name;
    QString senderUrl;
    QString senderUid;
    QString senderCallsign;
    QString sha256;
    qint64 sizeInBytes = -1;
};
struct CotChat
{
    QString messageId;
    QString chatroom;
    QString id;
    QString senderCallsign;
    QString parent;
    bool groupOwner = false;
};

// Typed readers for common TAK detail elements. They return empty values when the element
// is missing or its values are out of range. Links and URLs are data, never actions.
QString remarks(const Event &event);
QString contactEndpoint(const Event &event);
QList<CotLink> links(const Event &event);
QList<CotDestination> destinations(const Event &event);
std::optional<CotEmergency> emergency(const Event &event);
std::optional<CotFileShare> fileShare(const Event &event);
std::optional<CotChat> chat(const Event &event);
QString videoUrl(const Event &event);
QString iconsetPath(const Event &event);
// Reads a TAK color element, such as color, strokeColor, or fillColor. TAK stores a signed
// 32-bit ARGB integer in the argb or value attribute.
std::optional<QColor> detailColor(const Event &event, QStringView name);
std::optional<int> batteryPercent(const Event &event);
} // namespace swak

#pragma once
#include <QCollator>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace swak {
// SWAK uses two rules for text from servers, peers, files, and profiles.
//
// Free text, such as names, descriptions, and messages, keeps valid surrogate pairs, such as
// emoji. It loses control characters, format characters, and lone surrogates.
//
// Identity text, such as UIDs, callsigns, teams, and roles, has no surrogate at all. CoT event
// checks (eventError) use this rule, so an identity that passes it can travel in an event.

// True when the character is never valid in plain text.
inline bool unsafeCharacter(QChar ch)
{
    return ch.isNull() || ch.category() == QChar::Other_Control ||
           ch.category() == QChar::Other_Format;
}
enum class LineBreaks
{
    Remove,
    // Keep line feeds and tabs, for example in descriptions and chat messages.
    Keep
};
// Returns free text of at most limit UTF-16 code units, trimmed. It never splits a surrogate
// pair.
inline QString
plainText(const QString &text, qsizetype limit, LineBreaks breaks = LineBreaks::Remove)
{
    QString result;
    for (qsizetype i = 0; i < text.size() && result.size() < limit; ++i) {
        const auto ch = text.at(i);
        if (ch.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            if (result.size() + 2 <= limit) {
                result += ch;
                result += text.at(i + 1);
            }
            ++i;
        } else if (breaks == LineBreaks::Keep && (ch == u'\n' || ch == u'\t')) {
            result += ch;
        } else if (!unsafeCharacter(ch) && !ch.isSurrogate()) {
            result += ch;
        }
    }
    return result.trimmed();
}
// True when the text is a valid identity: not empty, at most limit code units, and without
// unsafe or surrogate characters.
inline bool identityText(const QString &text, qsizetype limit = 128)
{
    return !text.isEmpty() && text.size() <= limit &&
           std::none_of(text.cbegin(), text.cend(), [](QChar ch) {
               return unsafeCharacter(ch) || ch.isSurrogate();
           });
}
// Removes what identityText does not allow, for example from a server contact list. The result
// can be empty.
inline QString identityPlain(const QString &text, qsizetype limit = 128)
{
    QString result;
    for (const auto ch : text.left(limit)) {
        if (!unsafeCharacter(ch) && !ch.isSurrogate()) {
            result += ch;
        }
    }
    return result.trimmed();
}
// Qt shows a tooltip as rich text when it looks like markup. Remote and stored text must stay
// plain, so this helper escapes it. Line breaks stay line breaks.
inline QString plainToolTip(const QString &text)
{
    if (text.isEmpty()) {
        return {};
    }
    auto escaped = text.toHtmlEscaped();
    escaped.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    return QStringLiteral("<qt>") + escaped + QStringLiteral("</qt>");
}
// The keys of records, such as providers or collections, sorted by their "name" field with the
// locale order.
inline QStringList idsByName(const QJsonObject &records)
{
    auto ids = records.keys();
    const QCollator collator;
    std::sort(ids.begin(), ids.end(), [&records, &collator](const auto &a, const auto &b) {
        return collator.compare(
                   records.value(a).toObject().value(QStringLiteral("name")).toString(),
                   records.value(b).toObject().value(QStringLiteral("name")).toString()) < 0;
    });
    return ids;
}
// Checks the name, endpoint, and attribution of a map or data source: not empty, at most 100,
// 2048, and 300 characters, and only printable characters. boundsError names a missing or long
// value.
inline QString validateSourceText(const QJsonObject &source, const QString &boundsError)
{
    for (const auto &key :
         {QStringLiteral("name"), QStringLiteral("endpoint"), QStringLiteral("attribution")}) {
        const auto value = source.value(key).toString();
        const int limit = key == QStringLiteral("endpoint") ? 2048
                          : key == QStringLiteral("name")   ? 100
                                                            : 300;
        if (value.trimmed().isEmpty() || value.size() > limit) {
            return boundsError;
        }
        if (std::any_of(value.begin(), value.end(), [](QChar ch) {
                return !ch.isPrint();
            })) {
            return QObject::tr("Do not use control characters.");
        }
    }
    return {};
}
} // namespace swak

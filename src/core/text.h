#pragma once
#include <QString>

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
} // namespace swak

#pragma once
#include <QString>

namespace swak {
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

#include "core/marker_collections.h"
#include "core/marker_symbols.h"

#include <QObject>
namespace swak {
const QList<MarkerSymbol> &markerSymbols()
{
    static const auto symbols = [] {
        QList<MarkerSymbol> result;
        const QStringList identities{
            QStringLiteral("f"), QStringLiteral("h"), QStringLiteral("n"), QStringLiteral("u")};
        const QStringList names{QObject::tr("Friendly"),
                                QObject::tr("Hostile"),
                                QObject::tr("Neutral"),
                                QObject::tr("Unknown")};
        const QList<QColor> colors{
            QColor("#80e0ff"), QColor("#ff8080"), QColor("#aaffaa"), QColor("#ffff80")};
        const QStringList suffixes{QStringLiteral("G-U"),
                                   QStringLiteral("G-U-C-I"),
                                   QStringLiteral("G-U-C-A"),
                                   QStringLiteral("G-U-S-M")};
        const QStringList roles{QObject::tr("ground unit"),
                                QObject::tr("infantry"),
                                QObject::tr("armor"),
                                QObject::tr("medical unit")};
        for (int identity = 0; identity < 4; ++identity) {
            for (int role = 0; role < 4; ++role) {
                result.append(
                    {QStringLiteral("a-%1-%2").arg(identities.at(identity), suffixes.at(role)),
                     QObject::tr("%1 %2").arg(names.at(identity), roles.at(role)),
                     colors.at(identity)});
            }
        }
        return result;
    }();
    return symbols;
}
QJsonObject militaryMarkerCollection()
{
    QJsonArray symbols;
    for (const auto &symbol : markerSymbols()) {
        symbols.append(QJsonObject{
            {QStringLiteral("id"), symbol.code},
            {QStringLiteral("name"), symbol.name},
            {QStringLiteral("color"), symbol.color.name()},
            {QStringLiteral("frame"), encodeMarkerPath(markerFrame(symbol.code, {}, 1))},
            {QStringLiteral("glyph"), encodeMarkerPath(markerGlyph(symbol.code))}});
    }
    return {
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("id"), MilitaryMarkerCollection},
        {QStringLiteral("name"), QStringLiteral("MILSTD2525")},
        {QStringLiteral("description"),
         QObject::tr("Original vector symbols for a limited MIL-STD-2525C ground-unit subset.")},
        {QStringLiteral("version"), QStringLiteral("1.0.0")},
        {QStringLiteral("enabled"), true},
        {QStringLiteral("symbols"), symbols}};
}
} // namespace swak

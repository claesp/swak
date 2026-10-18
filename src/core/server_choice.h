#pragma once
#include "core/servers.h"
#include "core/tak_server_api.h"

#include <QComboBox>
#include <QJsonObject>

namespace swak {
// Fills a pane's server choice with the entries that reach martiPath, a path below /Marti
// such as /api/missions. Each item shows the entry name, or its identifier without a name. The
// selected entry stays selected when it is still in the list.
inline void fillTakServerChoice(QComboBox &choice, const Servers *servers, const QString &martiPath)
{
    const auto previous = choice.currentData().toString();
    choice.clear();
    if (!servers) {
        return;
    }
    const auto sources = servers->sources();
    for (auto it = sources.begin(); it != sources.end(); ++it) {
        const auto source = it.value().toObject();
        if (!TakServerApi::martiUrl(source, martiPath).isEmpty()) {
            const auto name = source.value(QStringLiteral("name")).toString();
            choice.addItem(name.isEmpty() ? it.key() : name, it.key());
        }
    }
    const auto index = choice.findData(previous);
    if (index >= 0) {
        choice.setCurrentIndex(index);
    }
}
} // namespace swak

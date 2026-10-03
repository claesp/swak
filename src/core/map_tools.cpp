#include "core/map_tools.h"
#include <QPainter>
#include <algorithm>
namespace swak {
void MapTools::add(MapTool *tool)
{
    if (!tool || tools_.contains(tool)) return;
    connect(tool, &MapTool::activationRequested, this, [this, tool] { emit activationRequested(tool); });
    tools_.append(tool); connect(tool, &MapTool::changed, this, &MapTools::changed);
    connect(tool, &QObject::destroyed, this, [this] {
        tools_.removeIf([](const auto &entry) { return entry.isNull(); }); emit changed();
    });
    connect(tool, &MapTool::overlayChanged, this, &MapTools::overlayChanged);
    emit changed();
}
bool MapTools::handle(QEvent &event, const Viewport &view, QWidget &map)
{
    const auto snapshot = tools_;
    for (bool active : {true, false})
        if (std::any_of(snapshot.cbegin(), snapshot.cend(), [&](const auto &tool) {
            return tool && tool->interactionActive() == active && tool->handle(event, view, map);
        })) return true;
    return false;
}
QList<MapItem> MapTools::itemsAt(const Viewport &view, QPointF position) const
{
    QList<MapItem> result;
    for (const auto &tool : tools_) if (tool) result.append(tool->itemsAt(view, position));
    return result;
}
bool MapTools::interact(const MapItem &item, QEvent &event, QWidget &map)
{
    const auto snapshot = tools_;
    return std::any_of(snapshot.cbegin(), snapshot.cend(), [&](const auto &tool) { return tool && tool->interact(item, event, map); });
}
void MapTools::paint(QPainter &painter, const Viewport &view) const
{
    for (const auto &tool : tools_) if (tool) { painter.save(); tool->paint(painter, view); painter.restore(); }
}
void MapTools::paintOverlay(QPainter &painter, const Viewport &view, QPointF pointer) const
{
    for (const auto &tool : tools_) if (tool) { painter.save(); tool->paintOverlay(painter, view, pointer); painter.restore(); }
}
bool MapTools::deleteSelection()
{
    return std::any_of(tools_.cbegin(), tools_.cend(), [](const auto &tool) { return tool && tool->deleteSelection(); });
}
bool MapTools::coordinateGridActive() const
{
    return std::any_of(tools_.cbegin(), tools_.cend(), [](const auto &tool) { return tool && tool->coordinateGridActive(); });
}
QString MapTools::statusText() const
{
    QStringList labels;
    for (const auto &tool : tools_) if (tool) {
        const auto text = tool->statusText(); if (!text.isEmpty()) labels.append(text);
    }
    return labels.join(QStringLiteral(" "));
}
bool MapTools::dismiss()
{
    return std::any_of(tools_.cbegin(), tools_.cend(), [](const auto &tool) { return tool && tool->dismiss(); });
}
bool MapTools::cancel()
{
    bool cancelled = false;
    for (const auto &tool : tools_) if (tool) cancelled = tool->cancel() || cancelled;
    return cancelled;
}
}

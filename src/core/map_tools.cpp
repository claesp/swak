#include "core/map_tools.h"

#include <QLineF>
#include <QPainter>

#include <algorithm>
#include <limits>
namespace swak {
void paintGuides(QPainter &painter, const Viewport &view, QPointF pointer, double opacity)
{
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(QColor(0, 0, 0, qRound(opacity * 255)), 1));
    painter.drawLine(QPointF(0, pointer.y()), QPointF(view.size.width(), pointer.y()));
    painter.drawLine(QPointF(pointer.x(), 0), QPointF(pointer.x(), view.size.height()));
}
double lineDistance(const QPolygonF &line, QPointF point)
{
    double nearest = std::numeric_limits<double>::infinity();
    for (qsizetype i = 1; i < line.size(); ++i) {
        const auto a = line.at(i - 1), d = line.at(i) - a;
        const double length = QPointF::dotProduct(d, d);
        const double t =
            length > 0 ? std::clamp(QPointF::dotProduct(point - a, d) / length, 0.0, 1.0) : 0;
        nearest = std::min(nearest, QLineF(point, a + t * d).length());
    }
    return nearest;
}
void MapTools::add(MapTool *tool)
{
    if (!tool || tools_.contains(tool)) {
        return;
    }
    connect(tool, &MapTool::activationRequested, this, [this, tool] {
        emit activationRequested(tool);
    });
    tools_.append(tool);
    connect(tool, &MapTool::changed, this, &MapTools::changed);
    connect(tool, &QObject::destroyed, this, [this] {
        tools_.removeIf([](const auto &entry) {
            return entry.isNull();
        });
        emit changed();
    });
    connect(tool, &MapTool::actionsChanged, this, &MapTools::actionsChanged);
    connect(tool, &MapTool::itemsChanged, this, &MapTools::itemsChanged);
    connect(tool, &MapTool::overlayChanged, this, &MapTools::overlayChanged);
    emit changed();
}
bool MapTools::handle(QEvent &event, const Viewport &view, QWidget &map)
{
    const auto snapshot = tools_;
    for (bool active : {true, false}) {
        if (std::any_of(snapshot.cbegin(), snapshot.cend(), [&](const auto &tool) {
                return tool && tool->interactionActive() == active &&
                       tool->handle(event, view, map);
            })) {
            return true;
        }
    }
    return false;
}
QList<MapItem> MapTools::itemsAt(const Viewport &view, QPointF position) const
{
    QList<MapItem> result;
    for (const auto &tool : tools_) {
        if (tool) {
            result.append(tool->itemsAt(view, position));
        }
    }
    return result;
}
bool MapTools::interact(const MapItem &item, QEvent &event, QWidget &map)
{
    const auto snapshot = tools_;
    return std::any_of(snapshot.cbegin(), snapshot.cend(), [&](const auto &tool) {
        return tool && tool->interact(item, event, map);
    });
}
void MapTools::populateInformationMenu(const MapItem &item, QMenu &menu)
{
    const auto snapshot = tools_;
    for (const auto &tool : snapshot) {
        if (tool) {
            tool->populateInformationMenu(item, menu);
        }
    }
}
QStringList MapTools::attributions() const
{
    QStringList credits;
    for (const auto &tool : tools_) {
        if (tool) {
            credits.append(tool->attributions());
        }
    }
    return credits;
}
double MapTools::areaFillOpacity() const
{
    for (const auto &tool : tools_) {
        if (tool) {
            if (const auto value = tool->areaFillOpacity()) {
                return *value;
            }
        }
    }
    return 0.3;
}
void MapTools::paint(QPainter &painter, const Viewport &view) const
{
    for (const auto &tool : tools_) {
        if (tool) {
            painter.save();
            tool->paint(painter, view);
            painter.restore();
        }
    }
}
void MapTools::paintOverlay(QPainter &painter, const Viewport &view, QPointF pointer) const
{
    for (const auto &tool : tools_) {
        if (tool) {
            painter.save();
            tool->paintOverlay(painter, view, pointer);
            painter.restore();
        }
    }
}
bool MapTools::deleteSelection()
{
    // A tool can destroy itself or another tool here. Iterate over a copy.
    const auto snapshot = tools_;
    return std::any_of(snapshot.cbegin(), snapshot.cend(), [](const auto &tool) {
        return tool && tool->deleteSelection();
    });
}
bool MapTools::guidesActive() const
{
    return std::any_of(tools_.cbegin(), tools_.cend(), [](const auto &tool) {
        return tool && tool->guidesActive();
    });
}
QString MapTools::statusText() const
{
    QStringList labels;
    for (const auto &tool : tools_) {
        if (tool) {
            const auto text = tool->statusText();
            if (!text.isEmpty()) {
                labels.append(text);
            }
        }
    }
    return labels.join(QStringLiteral(" "));
}
bool MapTools::dismiss()
{
    const auto snapshot = tools_;
    return std::any_of(snapshot.cbegin(), snapshot.cend(), [](const auto &tool) {
        return tool && tool->dismiss();
    });
}
void MapTools::populateContextMenu(const MapItem &item, QMenu &menu)
{
    const auto snapshot = tools_;
    for (const auto &tool : snapshot) {
        if (tool) {
            tool->populateContextMenu(item, menu);
        }
    }
}
bool MapTools::cancel()
{
    bool cancelled = false;
    const auto snapshot = tools_;
    for (const auto &tool : snapshot) {
        if (tool) {
            cancelled = tool->cancel() || cancelled;
        }
    }
    return cancelled;
}
} // namespace swak

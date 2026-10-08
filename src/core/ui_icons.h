#pragma once
#include "core/marker_collections.h"

#include <QIcon>
namespace swak {
enum class UiIcon
{
    Clear,
    Delete,
    Marker,
    Area,
    Map,
    Edit,
    Move,
    Circle,
    Distance,
    Line,
    Lock,
    Unlock,
    Server,
    Data,
    Confirm,
    Apply,
    Target,
    Route,
    RoutePlanning,
    Emergency
};
QIcon uiIcon(UiIcon icon);
QIcon collectionIcon(const CollectionSymbol &symbol, const QColor &color);
} // namespace swak

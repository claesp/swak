#pragma once
#include "core/marker_collections.h"
#include <QIcon>
namespace swak {
enum class UiIcon { Clear, Marker, Area, Map, Edit, Move, Circle, Distance, Line, Lock, Unlock };
QIcon uiIcon(UiIcon icon);
QIcon collectionIcon(const CollectionSymbol &symbol, const QColor &color);
}

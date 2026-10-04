#pragma once
#include "core/marker_collections.h"
#include <QIcon>
namespace swak {
enum class UiIcon { Clear, Marker, Area, Map, Edit, Move };
QIcon uiIcon(UiIcon icon);
QIcon collectionIcon(const CollectionSymbol &symbol, const QColor &color);
}

#pragma once
#include "core/cot_detail.h"
#include "core/event.h"

#include <vector>
namespace swak {
DecodeResult decodeCot(const QByteArray &data);
QByteArray encodeCot(const Event &event);
// The detail elements that encodeCot writes: the retained elements with the typed fields
// merged in. Typed fields replace only their own attributes.
std::vector<CotElement> mergedDetail(const Event &event);
} // namespace swak

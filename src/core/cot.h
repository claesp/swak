#pragma once
#include "core/event.h"
namespace swak {
DecodeResult decodeCot(const QByteArray &data);
QByteArray encodeCot(const Event &event);
}

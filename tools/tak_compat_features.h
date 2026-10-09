#pragma once
#include "core/event.h"

#include <QJsonObject>

namespace swak {
class Servers;
class Store;
} // namespace swak

// Runs the TAK Server feature checks with two connected clients. Each check records passed and
// a detail text. A failed check does not stop the later checks.
QJsonObject runFeatureChecks(swak::Servers &left,
                             swak::Servers &right,
                             swak::Store &leftStore,
                             swak::Store &rightStore,
                             const swak::Event &a,
                             const swak::Event &b,
                             const QJsonObject &profile);

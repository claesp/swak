#pragma once
#include <QRgb>

namespace swak {
// Colors of the Modern display mode. Painting code and the style sheet use these names.
namespace modern {
inline constexpr QRgb Background = 0xff0a0a0f;
inline constexpr QRgb Panel = 0xff12121c;
inline constexpr QRgb Raised = 0xff1b1b28;
inline constexpr QRgb Accent = 0xff00d4ff;
inline constexpr QRgb Text = 0xffe8eaed;
inline constexpr QRgb Dim = 0xff7d838c;
inline constexpr QRgb Alert = 0xffff3b3b;
// The fill of a map tile that is not loaded yet. It is lighter than the background, so the
// tile grid shows while the map loads.
inline constexpr QRgb MissingTile = 0xff10161e;
} // namespace modern
} // namespace swak

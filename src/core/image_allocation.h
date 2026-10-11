#pragma once
#include <QImage>

namespace swak {
// Creates an image for map tiles. The result is a null image when memory is short, so callers
// must check isNull. Use this function instead of the QImage constructor for every image whose
// failure the caller handles, so tests can cause that failure.
QImage allocateImage(int width, int height, QImage::Format format);

// The fault that allocateImage causes, for tests of the failure paths. The setting applies to
// all threads. Production code does not set it.
enum class ImageFault
{
    None,
    // allocateImage returns a null image, as QImage does when memory is short.
    NullImage,
    // allocateImage throws std::bad_alloc, as a failed allocation in other code can.
    Throw
};
void setImageFaultForTests(ImageFault fault);
} // namespace swak

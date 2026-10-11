#include "core/image_allocation.h"

#include <atomic>
#include <new>

namespace swak {
namespace {
std::atomic<ImageFault> imageFault{ImageFault::None};
static_assert(std::atomic<ImageFault>::is_always_lock_free);
} // namespace
QImage allocateImage(int width, int height, QImage::Format format)
{
    switch (imageFault.load(std::memory_order_relaxed)) {
    case ImageFault::None:
        break;
    case ImageFault::NullImage:
        return {};
    case ImageFault::Throw:
        throw std::bad_alloc();
    }
    return QImage(width, height, format);
}
// Only the tests call it. The analysis does not read the tests.
// cppcheck-suppress unusedFunction
void setImageFaultForTests(ImageFault fault)
{
    imageFault.store(fault, std::memory_order_relaxed);
}
} // namespace swak

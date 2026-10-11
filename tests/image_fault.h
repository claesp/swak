#pragma once
#include "core/image_allocation.h"

// Causes an image allocation fault until this object ends, also when a test fails early.
class ImageFaultGuard final
{
public:
    explicit ImageFaultGuard(swak::ImageFault fault)
    {
        swak::setImageFaultForTests(fault);
    }
    ~ImageFaultGuard()
    {
        swak::setImageFaultForTests(swak::ImageFault::None);
    }
    ImageFaultGuard(const ImageFaultGuard &) = delete;
    ImageFaultGuard &operator=(const ImageFaultGuard &) = delete;
};

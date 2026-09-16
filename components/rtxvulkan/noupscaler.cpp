#include "upscaler.hpp"

#include <components/rtx/error.hpp>

namespace Rtx
{
    std::unique_ptr<Upscaler> makeUpscaler(const Device&, VkInstance)
    {
        // Named rather than quietly ignored. A build that cannot upscale and renders at the output
        // size anyway is one whose frame times mean something else entirely.
        throw Unsupported("upscaling was asked for and this build has no DLSS; configure with -DOPENMW_RTX_DLSS=ON");
    }

    std::string describeUpscaling(const Device&, VkInstance)
    {
        return "not built in; configure with -DOPENMW_RTX_DLSS=ON";
    }

    std::span<const char* const> upscalerInstanceExtensions()
    {
        return {};
    }

    std::span<const char* const> upscalerDeviceExtensions()
    {
        return {};
    }
}

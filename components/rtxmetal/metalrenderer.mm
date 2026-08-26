#include "metalrenderer.hpp"

#import <Metal/Metal.h>

namespace Rtx
{
    std::unique_ptr<Renderer> createMetalRenderer(const RendererOptions&, std::string& reason)
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil)
        {
            reason = "no Metal device is installed";
            return nullptr;
        }

        // Named rather than worked around, in the spirit of the Vulkan side: what this renderer needs
        // is hardware traversal, and a device without it is a different renderer's problem.
        if (![device supportsRaytracing])
        {
            reason = std::string([[device name] UTF8String]) + " does not support ray tracing";
            return nullptr;
        }

        // **Metal 4, and so macOS 26.** That is where the command queue, the argument tables and the
        // denoised scaler live, and taking Metal 3 to widen the floor would be adopting a legacy path
        // on a fork that keeps none. Checked at runtime rather than by raising the whole binary's
        // deployment target, so an older machine gets a reason here instead of a crash at the first
        // Metal 4 call.
        if (@available(macOS 26.0, *))
        {
            if ([device newMTL4CommandQueue] == nil)
            {
                reason = "this device has no Metal 4 command queue";
                return nullptr;
            }
        }
        else
        {
            reason = "the Metal backend needs macOS 26 for Metal 4";
            return nullptr;
        }

        // Everything this backend needs is here and none of it is used yet. There is deliberately no
        // `Renderer` behind this: one that aborted on `setScene` would crash every caller that
        // believed the interface, starting with the pixel suite, and a reason is what the factory
        // exists to give instead.
        reason = "the Metal backend cannot trace yet";
        return nullptr;
    }
}

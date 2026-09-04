#include <cstdint>
#include <memory>
#include <string>

#include <components/rtx/renderer.hpp>

#include <SDL_video.h>

#ifdef OPENMW_RTX_VULKAN
#include <components/rtxvulkan/vulkanrenderer.hpp>
#endif

namespace Rtx
{
    std::uint32_t surfaceWindowFlag()
    {
#ifdef OPENMW_RTX_VULKAN
        return SDL_WINDOW_VULKAN;
#else
        return 0;
#endif
    }

    std::unique_ptr<Renderer> createRenderer([[maybe_unused]] const RendererOptions& options, std::string& reason)
    {
        // **This layer stands although one backend is left.** The core declares `createRenderer`
        // and cannot link a backend without a cycle, so somebody has to hold the answer — and every
        // consumer wants a renderer rather than a choice.
#ifdef OPENMW_RTX_VULKAN
        return createVulkanRenderer(options, reason);
#else
        reason = "this build has no ray tracing backend";
        return nullptr;
#endif
    }
}

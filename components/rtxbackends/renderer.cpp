#include <components/rtx/renderer.hpp>

#include <SDL_video.h>

#ifdef OPENMW_RTX_METAL
#include <components/rtxmetal/metalrenderer.hpp>
#endif

#ifdef OPENMW_RTX_VULKAN
#include <components/rtxvulkan/vulkanrenderer.hpp>
#endif

namespace Rtx
{
    std::uint32_t surfaceWindowFlag()
    {
#ifdef OPENMW_RTX_METAL
        return SDL_WINDOW_METAL;
#elif defined(OPENMW_RTX_VULKAN)
        return SDL_WINDOW_VULKAN;
#else
        return 0;
#endif
    }

    std::unique_ptr<Renderer> createRenderer([[maybe_unused]] const RendererOptions& options, std::string& reason)
    {
        // **No choice offered, because no machine has one.** A build has the backend its platform
        // runs natively and at most one that it merely compiles, and asking for the second would only
        // ever return the reason it cannot run. Which backend a machine develops is settled by what
        // it is, not by a flag.
#ifdef OPENMW_RTX_METAL
        return createMetalRenderer(options, reason);
#elif defined(OPENMW_RTX_VULKAN)
        return createVulkanRenderer(options, reason);
#else
        reason = "this build has no ray tracing backend";
        return nullptr;
#endif
    }
}

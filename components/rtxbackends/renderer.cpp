#include <cstdint>
#include <memory>

#include <SDL_video.h>

#include <components/rtx/error.hpp>
#include <components/rtx/renderer.hpp>

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

    std::unique_ptr<Renderer> createRenderer([[maybe_unused]] const RendererOptions& options)
    {
        // This layer stands although one backend is left: the core declares `createRenderer` and
        // cannot link a backend without a cycle, so somebody has to hold the answer.
#ifdef OPENMW_RTX_VULKAN
        return std::make_unique<VulkanRenderer>(options);
#else
        throw Unsupported("this build has no ray tracing backend");
#endif
    }
}

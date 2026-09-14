#include "createrenderer.hpp"

#include "vulkanrenderer.hpp"

namespace Rtx
{
    std::unique_ptr<Renderer> createVulkanRenderer(const RendererOptions& options)
    {
        return std::make_unique<VulkanRenderer>(options);
    }
}

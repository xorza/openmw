#pragma once

#include <memory>

#include <components/rtx/renderer.hpp>

namespace Rtx
{
    /// The one thing a host needs from this backend: a `Renderer` over Vulkan. Throws `Unsupported`
    /// where this machine cannot run it and `Error` where it should have, so a host can tell a
    /// machine to skip from a fault to report. Behind this the backend's headers are its own, and
    /// a fact about Vulkan reaches a host through nothing but the seam.
    std::unique_ptr<Renderer> createVulkanRenderer(const RendererOptions& options);
}

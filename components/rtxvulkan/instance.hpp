#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include <vulkan/vulkan_core.h>

#include <components/rtx/renderer.hpp>

#include "validation.hpp"

namespace Rtx
{
    /// A `VkInstance` and, when validation is on, the messenger and the log behind it.
    class Instance
    {
    public:
        /// @param validation the layers and what an error does, as the renderer was asked. A
        ///        developer feature: nobody enables it in a run they care about the frame rate of.
        /// @param surfaceExtensions what the window's surface needs, or empty for the headless path,
        ///        which is why `openmw-rtxtool` works over ssh.
        Instance(const ValidationOptions& validation, std::span<const char* const> surfaceExtensions);
        ~Instance();

        VkInstance getHandle() const { return mHandle; }

        /// Null unless validation was requested and the layer was present. Mutable through a
        /// const instance, because the debug callback writes it from whichever thread made the
        /// offending call.
        ValidationLog* getValidationLog() const { return mValidationLog.get(); }

        /// Whether `VK_KHR_surface_maintenance1` was loaded, which the device's swapchain half needs
        /// beside it. False for a headless instance, which has no surface to maintain.
        bool hasSurfaceMaintenance() const { return mSurfaceMaintenance; }

        /// Whether `VK_EXT_debug_utils` was enabled, which is what object names and command-buffer
        /// labels need. True whenever this build names objects, not only under validation — a
        /// capture is worth having without paying for the layers.
        bool hasDebugUtils() const { return mDebugUtils; }

        /// The version the loader reported, which is at least `sApiVersion`.
        std::uint32_t getApiVersion() const { return mApiVersion; }

    private:
        // Held by pointer so the address handed to the debug callback survives everything.
        bool mSurfaceMaintenance = false;
        std::unique_ptr<ValidationLog> mValidationLog;
        VkInstance mHandle = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
        std::uint32_t mApiVersion = 0;
        bool mDebugUtils = false;
    };
}

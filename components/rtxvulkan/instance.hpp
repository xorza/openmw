#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

        /// Whether `name` was loaded: what a device made on this instance reads an option's needs
        /// against, and a surface's `VK_KHR_surface` among them — loaded with a window and never
        /// headless, and what the device's swapchain rests on.
        bool hasExtension(std::string_view name) const;

        /// `vkGetPhysicalDeviceSurfaceCapabilities2KHR`, or null: the extended question of a
        /// surface, which is where a surface says which present modes the driver paces under
        /// (`LatencyPacer`). Null for a headless instance and for a loader without
        /// `VK_KHR_get_surface_capabilities2`.
        PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR getSurfaceCapabilities2() const
        {
            return mGetSurfaceCapabilities2;
        }

        /// Whether `VK_EXT_debug_utils` was enabled, which is what object names and command-buffer
        /// labels need. True whenever this build names objects, not only under validation — a
        /// capture is worth having without paying for the layers.
        bool hasDebugUtils() const { return hasExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); }

        /// The version the loader reported, which is at least `sApiVersion`.
        std::uint32_t getApiVersion() const { return mApiVersion; }

    private:
        /// Every extension loaded, as names of its own: a surface's are the window library's and
        /// the upscaler's are its runtime's, and neither promises its strings outlive the call.
        std::vector<std::string> mExtensions;

        // Held by pointer so the address handed to the debug callback survives everything.
        std::unique_ptr<ValidationLog> mValidationLog;
        VkInstance mHandle = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;

        /// Resolved with the create half, once, so the destructor does not ask the loader again.
        PFN_vkDestroyDebugUtilsMessengerEXT mDestroyMessenger = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR mGetSurfaceCapabilities2 = nullptr;
        std::uint32_t mApiVersion = 0;
    };
}

#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include <vulkan/vulkan_core.h>

#include <components/rtx/renderer.hpp>

#include "validation.hpp"

namespace Rtx
{
    /// Keeps the driver's own shader disk cache out of this process, for a process whose pictures
    /// are compared, so that every pipeline it makes is a compile of the SPIR-V. Before the first
    /// `Instance`, because the driver reads the word as it loads. The other half is that such a
    /// process keeps no `PipelineCache` of its own either: `RtxRenderer` says where.
    ///
    /// **Measured, because a pipeline out of a cache is not the compile's code.** Over
    /// `one-cell-walk` with the world held identical, a run whose launches came out of the
    /// driver's disk cache drew 59 of 360 pictures a part in 255 apart from a run that compiled
    /// them — single pixels anywhere in the frame, each one a value the two codes round across a
    /// byte's edge differently. Five cold compiles in five processes agreed to the byte, as did
    /// every run off the disk cache; and a run that loaded the fork's own blob started on the
    /// compile's code and was switched to the cache's a few seconds in, at frame 36, 41, 62 or
    /// 336 of its walk, the driver finishing the same work in the background. Those 59 pictures
    /// from frame 13, and the switches, are what the gate's repeat pairs failed on after a
    /// rebuild. A compile is one code and stays it, over thirty seconds of frames.
    ///
    /// The shell's word stands over this default, and the answer says whether it did: a run
    /// under a shell that turned the cache on is a run that may not repeat. The cache is
    /// NVIDIA's `__GL_SHADER_DISK_CACHE`, which its Vulkan driver reads as its GL one does —
    /// measured on Linux; whether the Windows driver reads it is not, which is what
    /// `VulkanRenderer` holds the launches' creation times against.
    bool refuseDriverShaderCache();

    /// Whether the driver was told to keep its shader disk cache out, by this process or by the
    /// shell: the word as the driver reads it, which is what `VulkanRenderer` holds the driver
    /// to when the launches come back too fast to have been compiled.
    bool driverShaderCacheRefused();

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

        /// Resolved with the create half, once, so the destructor does not ask the loader again.
        PFN_vkDestroyDebugUtilsMessengerEXT mDestroyMessenger = nullptr;
        std::uint32_t mApiVersion = 0;
        bool mDebugUtils = false;
    };
}

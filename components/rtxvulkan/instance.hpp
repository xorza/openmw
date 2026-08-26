#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/renderer.hpp>

#include "validation.hpp"

namespace Rtx
{
    struct InstanceOptions
    {
        /// Load `VK_LAYER_KHRONOS_validation`.
        ///
        /// A developer feature, and treated as one: with the layers on, `mPolicy` decides whether an
        /// error stops the process. Nobody enables this in a run they care about the frame rate of.
        bool mValidation = false;

        /// Catch missing barriers and wrong stage masks. Costs enough to be opt-in even among
        /// developers.
        bool mSynchronizationValidation = false;

        /// Instrument shaders to catch out-of-bounds descriptor access. Costs a great deal.
        bool mGpuAssistedValidation = false;

        ValidationPolicy mPolicy = ValidationPolicy::Abort;

        /// Surface extensions, when there is a window. Empty for the headless path, which is why
        /// `openmw-rtxtool` works over ssh.
        std::vector<const char*> mSurfaceExtensions;
    };

    /// The neutral instrumentation request as this backend takes it.
    ///
    /// Shared because the renderer and the window path both build an instance, and two copies of
    /// this would be two chances for a run to be validated differently than it was asked to be.
    InstanceOptions toInstanceOptions(const ValidationOptions& validation);

    /// A `VkInstance` and, when validation is on, the messenger and the log behind it.
    class Instance
    {
    public:
        explicit Instance(const InstanceOptions& options);
        ~Instance();

        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        VkInstance getHandle() const { return mHandle; }

        /// Null unless validation was requested and the layer was present.
        ///
        /// Mutable through a const instance on purpose: the log is a sink the debug callback writes
        /// to from whichever thread made the offending call, and that is not a property of the
        /// instance the way its handle is.
        ValidationLog* getValidationLog() const { return mValidationLog.get(); }

        /// Whether `VK_EXT_debug_utils` was enabled, which is what object names and command-buffer
        /// labels need. True whenever this build names objects, not only under validation — a
        /// capture is worth having without paying for the layers.
        bool hasDebugUtils() const { return mDebugUtils; }

        /// The version the loader reported, which is at least `sApiVersion`.
        std::uint32_t getApiVersion() const { return mApiVersion; }

    private:
        // Held by pointer so the address handed to the debug callback survives everything.
        std::unique_ptr<ValidationLog> mValidationLog;
        VkInstance mHandle = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT mMessenger = VK_NULL_HANDLE;
        std::uint32_t mApiVersion = 0;
        bool mDebugUtils = false;
    };
}

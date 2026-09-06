#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "deviceprofile.hpp"
#include "requirements.hpp"

namespace Rtx
{
    /// A physical device that qualified, what it reported about itself, and what this renderer
    /// decided from that.
    ///
    /// **This asks the device and `DeviceProfile` decides.** Every query is here so nothing asks
    /// twice; every judgement is there so a test can make one about a card nobody here owns.
    ///
    /// Selection is deliberately unforgiving: a device that lacks a required extension or feature is
    /// rejected with that name in the message rather than silently demoted to a lesser path. There
    /// is no lesser path.
    class PhysicalDevice
    {
    public:
        /// Picks a device, preferring discrete over anything else.
        ///
        /// Throws `Error` listing every candidate and what each was missing when none qualifies —
        /// the one moment where a wall of text is the useful answer.
        static PhysicalDevice select(VkInstance instance);

        VkPhysicalDevice getHandle() const { return mHandle; }

        const DeviceProperties& getProperties() const { return *mProperties; }

        /// What this renderer decided from those properties. `DeviceProfile` says why the two are
        /// different things.
        const DeviceProfile& getProfile() const { return mProfile; }

        /// Queue family with graphics and compute, which on the target hardware is also the one
        /// that can present. A separate transfer queue is an M12 question.
        std::uint32_t getQueueFamily() const { return mProfile.mQueueFamily; }

        /// Which of `getOptionalDeviceExtensions()` this device offers, in that order.
        const std::vector<const char*>& getAvailableOptionalExtensions() const { return mProfile.mOptionalExtensions; }

        /// Whether `name` is one of them.
        bool hasOptionalExtension(const char* name) const;

        /// Multi-line report for `openmw-rtxtool info`.
        std::string describe() const;

    private:
        PhysicalDevice() = default;

        VkPhysicalDevice mHandle = VK_NULL_HANDLE;

        // By pointer so a move leaves the internal pNext chain pointing at the same memory.
        std::unique_ptr<DeviceProperties> mProperties;

        DeviceProfile mProfile;
    };
}

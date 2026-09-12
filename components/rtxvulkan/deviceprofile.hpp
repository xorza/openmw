#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "requirements.hpp"

namespace Rtx
{
    /// What this renderer will do on a device, worked out once from what the device says of
    /// itself. Every question about the card is asked here and nowhere else, because this fork
    /// targets cards nobody here owns: `profileOf` is a function of the device's own answers, so
    /// a test hands it an RTX 2060's heaps and asks what the renderer would do.
    struct DeviceProfile
    {
        /// What stops this renderer running here, named, or empty where nothing does.
        ///
        /// **Every other field means something only when this is empty.**
        std::string mObstacle;

        /// The queue family that can do everything this renderer submits.
        std::uint32_t mQueueFamily = 0;

        /// The largest heap carrying memory the host writes into and the device reads: what
        /// decides where the scene's tables live. Every card this fork targets offers such a type;
        /// what differs is the room behind it — all of video memory where the firmware maps it,
        /// and about 246 MiB where it does not.
        VkDeviceSize mHostWrittenBytes = 0;

        /// Which of `getOptionalDeviceExtensions()` this device offers, in that order. The
        /// pointers are the static list's own, so they outlive every device.
        std::vector<const char*> mOptionalExtensions;

        /// How many bits of the device's clock the chosen queue writes into a timestamp, or nought
        /// where it writes none, so that queue reports nothing rather than something wrong.
        std::uint32_t mTimestampBits = 0;
    };

    /// Reads a device's own answers into the decisions this renderer makes from them. Nothing here
    /// calls Vulkan, so a test can hand it a card that is not plugged in. `supported` is mutable
    /// for the reason `findMissingFeatures` is; nothing writes to it.
    DeviceProfile profileOf(const DeviceProperties& properties, DeviceFeatures& supported,
        std::span<const std::string> extensions, std::span<const VkQueueFamilyProperties> queues);
}

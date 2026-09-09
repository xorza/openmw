#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "requirements.hpp"

namespace Rtx
{
    /// What this renderer will do on a device, worked out once from what the device says of itself.
    ///
    /// **Every question about the card is asked here and nowhere else.** A check spelled at the
    /// place that needs the answer is a check nothing can test without the hardware, and this fork
    /// targets cards nobody here owns: `profileOf` is a function of the device's own answers, so a
    /// test hands it an RTX 2060's heaps and reordering hint and asks what the renderer would do.
    ///
    /// **A number the device states is not a decision.** The properties say the reordering hint is
    /// `NONE` and that the host-visible heap is 246 MiB; what this says is that a sort buys nothing
    /// and how much room a `Buffer::hostWritten` has. The difference is the whole point of the type.
    struct DeviceProfile
    {
        /// What stops this renderer running here, named, or empty where nothing does.
        ///
        /// **Every other field means something only when this is empty.**
        std::string mObstacle;

        /// The queue family that can do everything this renderer submits.
        std::uint32_t mQueueFamily = 0;

        /// Whether asking for a reorder does anything.
        ///
        /// **A device that answers `NONE` is not refused.** Ada added the hardware; Turing and
        /// Ampere expose the extension and reorder nothing, so the calls compile and cost what a
        /// call costs. `Rtx::Reorder` is off by default because every mode measured slower even on
        /// the hardware that has it, so what this gates is a run that asked for one by name.
        bool mReorders = false;

        /// The largest heap carrying memory the host writes into and the device reads.
        ///
        /// **The figure that decides where the scene's tables live, and it is not a yes or no.**
        /// Every card this fork targets offers such a type; what differs is the room behind it —
        /// the whole of video memory where the firmware maps it, and about 246 MiB where it does
        /// not. A test of the property bits alone answers the wrong question.
        VkDeviceSize mHostWrittenBytes = 0;

        /// Which of `getOptionalDeviceExtensions()` this device offers, in that order.
        ///
        /// **A decision like any other**, and one the caller would otherwise make by walking the
        /// same list against the same answers. The pointers are the static list's own, so they
        /// outlive every device.
        std::vector<const char*> mOptionalExtensions;

        /// How many bits of the device's clock the chosen queue writes into a timestamp, or nought
        /// where it writes none.
        ///
        /// **Every per-pass figure this renderer reports is a pair of these**, so a queue that
        /// cannot write them reports nothing rather than something wrong. Read here because the
        /// queue families are read here, and asking twice for an answer settled before the window
        /// existed is the sort of waste that spreads.
        std::uint32_t mTimestampBits = 0;
    };

    /// Reads a device's own answers into the decisions this renderer makes from them.
    ///
    /// Every argument is what the device reported, so nothing here calls Vulkan and a test can hand
    /// it a card that is not plugged in.
    ///
    /// `supported` is mutable for the reason `findMissingFeatures` is; nothing writes to it.
    DeviceProfile profileOf(const DeviceProperties& properties, DeviceFeatures& supported,
        std::span<const std::string> extensions, std::span<const VkQueueFamilyProperties> queues);
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/renderer.hpp>

#include "owned.hpp"

namespace Rtx
{
    class Device;

    /// Timestamps written into the command stream, so a frame can say where its device time went,
    /// where a wall clock around a submit measures one number for eight pieces of work. Both ends
    /// wait for every stage, so a zone cannot overlap its neighbours — which would distort a
    /// renderer whose passes overlap, and this one puts a full barrier between every pass already.
    /// Zones may span several command buffers, because each reserves and resets only the pair of
    /// queries it writes.
    class GpuTimer
    {
    public:
        /// The most zones one frame may open. Thirteen are used; the rest is room to bisect one.
        static constexpr std::uint32_t sMaxZones = 24;

        explicit GpuTimer(const Device& device);

        /// Forgets the last frame's zones. Whatever is opened after this is one report.
        void beginFrame();

        /// Opens a zone. `name` is stored rather than copied, so it must outlive the frame. Also
        /// names the region for a capture, where the build and the instance carry the labels.
        void open(VkCommandBuffer commands, std::string_view name);

        /// Closes the zone `open` started. Every open is closed before the next is opened.
        void close(VkCommandBuffer commands);

        /// What the zones measured, in the order they were opened. The caller waited for every
        /// submit the zones were recorded into. Valid until the next `beginFrame`.
        std::span<const GpuSpan> resolve();

    private:
        const Device& mDevice;
        Owned<VkQueryPool, vkDestroyQueryPool> mHandle;

        /// Nanoseconds a tick of the device's clock is worth, and how many of its bits count.
        double mPeriod = 1.0;
        std::uint64_t mMask = ~std::uint64_t{ 0 };
        bool mSupported = false;

        /// A name and the first of the two queries bracketing it.
        struct Zone
        {
            std::string_view mName;
            std::uint32_t mFirstQuery = 0;
        };

        std::vector<Zone> mZones;
        std::vector<GpuSpan> mSpans;

        /// Which of `mZones` is open, or `mZones.size()` for none.
        std::size_t mOpen = 0;
    };

    /// Brackets a piece of work where there is a timer to bracket it with. A scene arriving and a
    /// picture inside the interface record the same commands and are not frames, so zones opened
    /// there would land in whichever frame report came next.
    inline void openZone(GpuTimer* timer, VkCommandBuffer commands, std::string_view name)
    {
        if (timer != nullptr)
            timer->open(commands, name);
    }

    inline void closeZone(GpuTimer* timer, VkCommandBuffer commands)
    {
        if (timer != nullptr)
            timer->close(commands);
    }
}

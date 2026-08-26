#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/renderer.hpp>

namespace Rtx
{
    class Device;

    /// Timestamps written into the command stream, so a frame can say where its device time went.
    ///
    /// **A wall clock around a submit measures one number for eight pieces of work.** The frame is a
    /// ray tracing dispatch, a wavelet, a composite, an upscaler and two tone passes, and the
    /// placement before it is two acceleration-structure builds in two more submits — all of which
    /// the CPU sees as "the queue was busy". These are what each of them cost.
    ///
    /// **Both ends wait for everything.** A timestamp is written when prior commands have reached
    /// the stage named, and the stage here is every one of them: so a zone begins when the work
    /// before it has finished and ends when its own has, which is a span that cannot overlap its
    /// neighbours. That would distort a renderer whose passes overlap; this one puts a full barrier
    /// between every pass already, so there is nothing to distort.
    ///
    /// Zones may span several command buffers — the placement records two submits of its own before
    /// the frame's — because each reserves and resets only the pair of queries it writes.
    class GpuTimer
    {
    public:
        /// The most zones one frame may open. Eight are used; the rest is room to bisect one.
        static constexpr std::uint32_t sMaxZones = 24;

        explicit GpuTimer(const Device& device);
        ~GpuTimer();

        GpuTimer(const GpuTimer&) = delete;
        GpuTimer& operator=(const GpuTimer&) = delete;

        /// Whether this device and queue can timestamp at all. Everything below is a no-op when not.
        bool isSupported() const { return mSupported; }

        /// Forgets the last frame's zones. Whatever is opened after this is one report.
        void beginFrame();

        /// Opens a zone. `name` is stored rather than copied, so it must outlive the frame — which a
        /// literal does and nothing else here is.
        ///
        /// Also names the region for a capture, where the build and the instance carry the labels:
        /// the same bracket serves an external profiler, which is where the counters a timestamp
        /// cannot give you live.
        void open(VkCommandBuffer commands, std::string_view name);

        /// Closes the zone `open` started. Every open is closed before the next is opened.
        void close(VkCommandBuffer commands);

        /// What the zones measured, in the order they were opened.
        ///
        /// The caller has waited for every submit the zones were recorded into — which this renderer
        /// does by construction, since each of the three fences before returning. Valid until the
        /// next `beginFrame`.
        std::span<const GpuSpan> resolve();

    private:
        const Device& mDevice;
        VkQueryPool mHandle = VK_NULL_HANDLE;

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
}

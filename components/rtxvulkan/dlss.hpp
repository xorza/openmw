#pragma once

#include <span>
#include <string>

#include <vulkan/vulkan_core.h>

#include <components/rtx/upscale.hpp>

// NGX's own, `typedef struct X X` and not defined here: the SDK's headers are private to this
// target, and a test that builds a feature must be able to include this one.
struct NVSDK_NGX_Parameter;

namespace Rtx
{
    class Device;

    /// Whether Ray Reconstruction can run here, and why not where it cannot.
    struct DlssSupport
    {
        bool mAvailable = false;

        /// Empty where it is available.
        std::string mObstacle;
    };

    /// The process's NGX runtime, and what it says this machine can do with it. Ray Reconstruction
    /// is an upscaler that denoises across several frames from what the G-buffer already carries,
    /// where the à-trous pass has one frame and one channel. One at a time, and the constructor
    /// refuses a second: NGX's state is global to the process and `NVSDK_NGX_VULKAN_Shutdown` is
    /// unconditional, so a second built to ask a question and let go again would shut down the
    /// runtime the renderer is upscaling with — `probe` hands back an answer rather than a runtime.
    /// Absent without `-DOPENMW_RTX_DLSS=ON`.
    class Dlss
    {
    public:
        /// What NGX needs enabled on the instance and on the device, asked before either exists,
        /// which is why it is static.
        static std::span<const char* const> getInstanceExtensions();
        static std::span<const char* const> getDeviceExtensions();

        /// Whether Ray Reconstruction can run on `device`, without leaving a runtime behind: where
        /// one is already up this asks it, and where none is, it stands one up for the length of
        /// the call. Throws `Error` where the runtime will not come up at all.
        static DlssSupport probe(const Device& device, VkInstance instance);

        /// Starts the runtime. **Throws where one is already up**, on any device: there is one per
        /// process, and a second would end the first rather than stand beside it.
        Dlss(const Device& device, VkInstance instance);

        ~Dlss();

        Dlss(const Dlss&) = delete;
        Dlss& operator=(const Dlss&) = delete;

        /// Whether Ray Reconstruction can run here, which is a question about the driver as much as
        /// the hardware — NGX answers it after it has loaded its feature libraries and looked.
        bool isAvailable() const { return mAvailable; }

        /// Why not, where it cannot. Empty where it can.
        const std::string& getObstacle() const { return mObstacle; }

        /// What to render at to produce `output` under `upscale`, which must not be `Off` — DLSS's
        /// answer and not a ratio applied here, because a frame traced at anything else is a frame
        /// it will refuse. Throws `Error` where DLSS will not answer.
        VkExtent2D getRenderSize(VkExtent2D output, Upscale upscale) const;

        /// The device NGX was brought up on, which is the one a feature is built for.
        VkDevice getDevice() const { return mDevice; }

    private:
        /// Whichever one is up, or null. **A tripwire and not an owner**: nothing reads it to find
        /// the runtime — the renderer holds that — and what it is for is making a second one a throw
        /// instead of a silent shutdown of the first.
        static inline Dlss* sLive = nullptr;

        VkDevice mDevice = VK_NULL_HANDLE;
        NVSDK_NGX_Parameter* mCapabilities = nullptr;
        bool mAvailable = false;
        std::string mObstacle;
    };
}

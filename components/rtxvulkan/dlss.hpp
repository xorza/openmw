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

    /// The process's NGX runtime, and what it says this machine can do with it.
    ///
    /// **Ray Reconstruction is an upscaler that denoises**, which is why it is worth a dependency
    /// rather than a filter of our own: it reconstructs from the demodulated radiance, albedo,
    /// normals, depth and motion the G-buffer already carries, across several frames, where the
    /// à-trous pass has one frame and one channel to work with.
    ///
    /// **Owned outright, at a place that says so.** `VulkanRenderer` builds one in its constructor
    /// when it was asked to upscale and destroys it in its destructor, and that is the whole of the
    /// lifetime — no counting, nothing lazy, and nothing that comes up as a side effect of being
    /// asked a question. A renderer that does not upscale never has one.
    ///
    /// **One at a time, and the constructor refuses a second.** NGX's state is global to the
    /// process, `NVSDK_NGX_VULKAN_Shutdown` is unconditional, and the runtime belongs to the
    /// `VkDevice` it was started on — so a second of these would not coexist with the first, it
    /// would end it. That was a real bug: `describeDevice` used to build one to ask "is Ray
    /// Reconstruction available" and let it go again, which shut down the runtime the renderer was
    /// still upscaling with, surfacing as `FAIL_NotInitialized` from a frame a cell load later,
    /// pointing at nothing. `probe` is what that question asks now, and it hands back an answer
    /// rather than a runtime.
    ///
    /// **Not thread safe, and every caller is on one thread**: a renderer is built before there is
    /// anything else to build one from.
    ///
    /// Built only with `-DOPENMW_RTX_DLSS=ON`, which needs the SDK; the whole class is absent
    /// otherwise, so nothing has to ask at runtime whether it was compiled in.
    class Dlss
    {
    public:
        /// What NGX needs enabled on the instance and on the device, asked before either exists.
        ///
        /// **Static, and that is the only reason this can work.** The extensions have to be turned
        /// on when the instance and device are created, which is long before there is anything to
        /// initialise NGX with — so the SDK answers this without either.
        static std::span<const char* const> getInstanceExtensions();
        static std::span<const char* const> getDeviceExtensions();

        /// Whether Ray Reconstruction can run on `device`, without leaving a runtime behind.
        ///
        /// **An answer and not a handle**, which is the whole of why this is not the constructor:
        /// asking a capability question must not decide anything about who owns NGX. Where a runtime
        /// is already up this asks that one; where none is, it stands one up for the length of the
        /// call and takes it down again.
        ///
        /// Throws `Error` where the runtime will not come up at all, which a caller reporting on a
        /// device wants to print rather than propagate.
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

        /// What to render at to produce `output` under `upscale`, which must not be `Off`. Throws
        /// `Error` where DLSS will not answer, which is the same condition as it being unavailable.
        ///
        /// **DLSS's answer and not a ratio applied here.** The feature library picks the size, it
        /// has changed it between versions, and a frame traced at anything else is a frame it will
        /// refuse.
        VkExtent2D getRenderSize(VkExtent2D output, Upscale upscale) const;

        /// The device NGX was brought up on, which is the one a feature is built for.
        VkDevice getDevice() const { return mDevice; }

        /// NGX's own map, which answers questions. **Not the map a feature is built from** — that
        /// one is allocated per feature and belongs to it.
        NVSDK_NGX_Parameter* getCapabilities() const { return mCapabilities; }

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

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/latencyreport.hpp>
#include <components/rtx/pacing.hpp>
#include <components/sdlutil/vsyncmode.hpp>

#include "handles.hpp"
#include "latencypacer.hpp"
#include "pacedmodes.hpp"

struct SDL_Window;

namespace Rtx
{
    class Device;
    class Image;
    class Instance;
    class Swapchain;

    /// The surface, the swapchain, and everything that keeps a frame from overtaking the one in
    /// front of it: a semaphore per swapchain image and not per frame in flight, the timeline
    /// value of the last blit onto each image because mailbox hands one back before the
    /// presentation engine has finished with it, the sync objects rebuilt when a recreate returns
    /// a different image count, and a `waitIdle` before any of them are destroyed. The renderer
    /// never draws into a swapchain image: it blits, because the format a surface offers is not
    /// one a compute shader may store to.
    class Presenter
    {
    public:
        /// What SDL says an instance needs before this window can have a surface. Static, because
        /// the instance has to be created with these enabled before the surface can be made.
        static std::vector<const char*> getInstanceExtensions(SDL_Window* window);

        /// Throws `Unsupported` where the surface or the swapchain will not come up. The blit is a
        /// submit of the device's pool like any other, so it signals the timeline and carries what
        /// was deferred ahead of it — a submit of its own that took a timeline value would let the
        /// graveyard free what a deferred batch names before it ran.
        Presenter(const Device& device, const Instance& instance, SDL_Window* window, SDLUtil::VSyncMode verticalSync,
            const Pacing& pacing);
        ~Presenter();

        /// Blits `frame`, in `VK_IMAGE_LAYOUT_GENERAL` and left there, onto the next swapchain
        /// image and queues it. A surface that no longer matches the window is not an error: the
        /// swapchain is marked stale, the one record of it, and `wantsResize` answers yes. A
        /// present the driver paces is marked around the call and carries its id; one that owes a
        /// sleep pays it first.
        void present(const Image& frame);

        /// The driver's pacing, forwarded — `Renderer::pacesFrames` and the three beside it. The
        /// pacer is this object's because it is the swapchain's: a rebuild and a mode change
        /// both tell it where it stands.
        bool pacesFrames() const { return mPacer.isLive(); }

        /// Costs a rebuild where the mode moves the present mode — the vertical sync's `Disabled`
        /// is immediate where the player asks for the pacing and mailbox where not — and nothing
        /// otherwise.
        void setPacing(const Pacing& pacing);
        void awaitFrame();
        void endSimulation(bool flash) { mPacer.endSimulation(flash); }
        std::optional<LatencyReport> describeLatency() const { return mPacer.describeLatency(); }

        /// Whether the swapchain has to be remade to show `extent`. Split from `rebuild` because a
        /// rebuild frees the command buffers a handed-over batch may be sitting beside, so the
        /// caller has to drain first, and that drain costs more than the rebuild it guards. Not
        /// const, because a hidden window takes no swapchain and the staleness carries the rebuild
        /// to the frame the window comes back on.
        bool wantsResize(VkExtent2D extent);

        /// Remakes the swapchain at `extent`, unconditionally. Waits for everything in flight, so
        /// it is a stall by construction — ask `wantsResize` first.
        void rebuild(VkExtent2D extent);

        /// Says how the presented image should meet the refresh, rebuilding only where that changes
        /// the mode the surface will actually run in.
        void setVerticalSync(SDLUtil::VSyncMode mode);

        VkExtent2D getExtent() const;

    private:
        /// Two semaphores and one command buffer per swapchain image, and a present fence where the
        /// device offers one, for the swapchain as it now stands. `releaseImageSync` comes first.
        void makeImageSync();

        /// Destroys what `makeImageSync` made. The caller owes the `waitIdle` before it, and a
        /// swapchain it guards stays up until this returns.
        void releaseImageSync();

        void destroy();

        /// Remakes the swapchain at `extent` with everything that hangs off it: the sync objects,
        /// the pacer's swapchain and the pool's id. Waits for everything in flight first.
        void remake(VkExtent2D extent);

        /// Points the pacer at the swapchain as it now stands: after every creation of it.
        void followSwapchain();

        /// Tells the pool the id the next submit carries, after every step of the pacer's that
        /// moves it: a follow, a sleep, a present.
        void passPresentId();

        const Device& mDevice;
        VkInstance mInstance = VK_NULL_HANDLE;
        VkSurfaceKHR mSurface = VK_NULL_HANDLE;

        /// Which present modes the surface paces under, read once the surface exists and before
        /// the swapchain, which is made under one of them or not.
        PacedModes mPacedModes;

        /// By pointer because it is built from `mSurface`, which cannot exist before the
        /// constructor's body.
        std::unique_ptr<Swapchain> mSwapchain;

        /// What the driver's sleep signals: the pacer's for as long as it lives, and never the
        /// queue's clock (`LatencyPacer`).
        Semaphore mSleepSemaphore;
        LatencyPacer mPacer;

        /// What an acquire signals and the blit behind it waits.
        struct Acquisition
        {
            Semaphore mSemaphore;

            /// What the blit that took this slot signalled on the timeline, or nought until
            /// something takes it.
            std::uint64_t mBlit = 0;
        };

        /// One per swapchain image, taken in turn and never indexed by the image, which an acquire
        /// cannot be keyed on. A semaphore handed to `vkAcquireNextImageKHR` must carry no operation
        /// still pending, and the acquire's signal stays pending until the blit that waits it has
        /// run — behind a whole frame of tracing — so one semaphore for every acquire is
        /// `VUID-vkAcquireNextImageKHR-semaphore-01779`. A slot comes free when the timeline has
        /// passed its blit.
        std::vector<Acquisition> mAcquiring;

        /// Which slot the next acquire takes.
        std::uint32_t mAcquisition = 0;

        /// What one swapchain image is presented through.
        struct SwapImage
        {
            /// Signalled by the blit and waited by the present. Per image and not one: a present
            /// may still be reading the semaphore a frame signalled, and there is no fence that says
            /// when it stopped.
            Semaphore mRendered;

            /// What the last blit onto the image signalled on the timeline, so it is not written
            /// again while its present is still outstanding.
            std::uint64_t mBlitOn = 0;

            /// What the presentation engine signals when it has finished with the image, where the
            /// device offers `VK_KHR_swapchain_maintenance1` — the only thing that says a present
            /// is over, since a queue-idle proves the queue is empty rather than that the
            /// compositor has let go, and the one thing here the timeline cannot say. A present
            /// rejected with `VK_ERROR_OUT_OF_DATE_KHR` still signals its fence, so waiting on it is
            /// safe. Null where the device offers none.
            Fence mPresented;

            /// Out of the device's pool, which allows a buffer to be reset by beginning it again.
            VkCommandBuffer mCommands = VK_NULL_HANDLE;
        };

        /// One per swapchain image, indexed by the image the acquire answered with.
        std::vector<SwapImage> mImages;

        /// Whether the surface stopped matching the window since the last rebuild. An acquire or a
        /// present can fail at a size nothing asked to change, and a resize that only rebuilt when
        /// the extent differed would leave that one unrecoverable.
        bool mStale = false;
    };
}

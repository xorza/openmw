#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/sdlutil/vsyncmode.hpp>

#include "owned.hpp"

struct SDL_Window;

namespace Rtx
{
    class CommandPool;
    class Device;
    class Image;
    class Swapchain;

    /// The surface, the swapchain, and everything that keeps a frame from overtaking the one in
    /// front of it: a semaphore per swapchain image and not per frame in flight, a fence per image
    /// because mailbox hands one back before the presentation engine has finished with it, the
    /// sync objects rebuilt when a recreate returns a different image count, and a `waitIdle`
    /// before any of them are destroyed. The renderer never draws into a swapchain image: it
    /// blits, because the format a surface offers is not one a compute shader may store to.
    class Presenter
    {
    public:
        /// What SDL says an instance needs before this window can have a surface. Static, because
        /// the instance has to be created with these enabled before the surface can be made.
        static std::vector<const char*> getInstanceExtensions(SDL_Window* window);

        /// Throws `Error` where the surface or the swapchain will not come up.
        Presenter(const Device& device, VkInstance instance, SDL_Window* window);
        ~Presenter();

        /// Blits `frame`, in `VK_IMAGE_LAYOUT_GENERAL` and left there, onto the next swapchain
        /// image and queues it. False where the surface no longer matches the window, which is not
        /// an error: the caller resizes and asks again.
        bool present(const Image& frame);

        /// Waits until the present that last read `frame` has finished with it. A present's blit
        /// outlives the call that queued it — under FIFO it waits until the presentation engine has
        /// let that swapchain image go — and no barrier's source scope reaches across a submit. A
        /// no-op for an image this has never presented.
        void waitForLastUse(const Image& frame);

        /// Whether the swapchain has to be remade to show `extent`. Split from `rebuild` because a
        /// rebuild resets the command pool, so the caller has to drain first, and that drain costs
        /// more than the rebuild it guards. Not const, because a hidden window takes no swapchain
        /// and the staleness carries the rebuild to the frame the window comes back on.
        bool wantsResize(VkExtent2D extent);

        /// Remakes the swapchain at `extent`, unconditionally. Waits for everything in flight, so
        /// it is a stall by construction — ask `wantsResize` first.
        void rebuild(VkExtent2D extent);

        /// Says how the presented image should meet the refresh, rebuilding only where that changes
        /// the mode the surface will actually run in.
        void setVerticalSync(SDLUtil::VSyncMode mode);

        VkExtent2D getExtent() const;

    private:
        /// Two semaphores, one fence and one command buffer per swapchain image, made again
        /// whenever the count changes.
        void remakeImageSync();

        /// Destroys what `remakeImageSync` made. The caller owes the `waitIdle` before it.
        void releaseImageSync();

        /// Records that `image` was read by the present `fence` will signal.
        void rememberUse(VkImage image, VkFence fence);

        void destroy();

        const Device& mDevice;
        VkInstance mInstance = VK_NULL_HANDLE;
        VkSurfaceKHR mSurface = VK_NULL_HANDLE;

        /// By pointer because it is built from `mSurface`, which cannot exist before the
        /// constructor's body.
        std::unique_ptr<Swapchain> mSwapchain;

        /// What an acquire signals and the blit behind it waits.
        struct Acquisition
        {
            Owned<VkSemaphore, vkDestroySemaphore> mSemaphore;

            /// Signalled once the blit that took this slot has run. Null until something takes it.
            VkFence mBlit = VK_NULL_HANDLE;
        };

        /// One per swapchain image, taken in turn and never indexed by the image, which an acquire
        /// cannot be keyed on. A semaphore handed to `vkAcquireNextImageKHR` must carry no operation
        /// still pending, and the acquire's signal stays pending until the blit that waits it has
        /// run — behind a whole frame of tracing — so one semaphore for every acquire is
        /// `VUID-vkAcquireNextImageKHR-semaphore-01779`. A slot comes free when its blit's fence
        /// signals.
        std::vector<Acquisition> mAcquiring;

        /// Which slot the next acquire takes.
        std::uint32_t mAcquisition = 0;

        /// Signalled by the blit and waited by the present. Per swapchain image and not one: a
        /// present may still be reading the semaphore a frame signalled, and there is no fence that
        /// says when it stopped.
        std::vector<Owned<VkSemaphore, vkDestroySemaphore>> mRendered;

        /// What the last blit onto each image signalled, so one is not written again while its
        /// present is still outstanding.
        std::vector<Owned<VkFence, vkDestroyFence>> mPresenting;

        /// What the presentation engine signals when it has finished with each image, where the
        /// device offers `VK_KHR_swapchain_maintenance1` — the only thing that says a present is
        /// over, since a queue-idle proves the queue is empty rather than that the compositor has
        /// let go. A present rejected with `VK_ERROR_OUT_OF_DATE_KHR` still signals its fence, so
        /// waiting on every one of these is safe.
        std::vector<Owned<VkFence, vkDestroyFence>> mPresented;

        std::vector<VkCommandBuffer> mCommands;

        /// Which present a frame image was last read by. One entry per image the renderer alternates
        /// between, so a linear scan is the whole lookup.
        struct LastUse
        {
            VkImage mImage = VK_NULL_HANDLE;
            VkFence mFence = VK_NULL_HANDLE;
        };
        std::vector<LastUse> mLastUse;

        /// Whether the surface stopped matching the window since the last rebuild. An acquire or a
        /// present can fail at a size nothing asked to change, and a resize that only rebuilt when
        /// the extent differed would leave that one unrecoverable.
        bool mStale = false;

        /// Its own, because these are re-recorded every frame and the renderer's pool is shaped for
        /// setup work that submits and waits.
        std::unique_ptr<CommandPool> mPool;
    };
}

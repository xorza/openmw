#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan_core.h>

struct SDL_Window;

namespace Rtx
{
    class CommandPool;
    class Device;
    class Image;
    class Swapchain;

    /// The surface, the swapchain, and everything that keeps a frame from overtaking the one in
    /// front of it.
    ///
    /// **All of it here rather than in whoever owns the window.** A caller driving a swapchain
    /// itself has to get the same handful of things right — a semaphore per swapchain image and not
    /// per frame in flight, a fence per image because mailbox hands one back before the presentation
    /// engine has finished with it, the acquire's stage named in the first barrier's source scope,
    /// the sync objects rebuilt when a recreate returns a different image count, and a `waitIdle`
    /// before any of them are destroyed. The one caller that had done all that was also the only
    /// place that could get it wrong.
    ///
    /// The renderer never draws into a swapchain image. It renders into one of its own and blits,
    /// because the format a surface offers is not one a compute shader may store to.
    class Presenter
    {
    public:
        /// What SDL says an instance needs before this window can have a surface.
        ///
        /// **Static, and asked before the instance exists**, which is the only order that works: the
        /// instance has to be created with these enabled, and the surface cannot be made until it
        /// has been.
        static std::vector<const char*> getInstanceExtensions(SDL_Window* window);

        /// Throws `Error` where the surface or the swapchain will not come up.
        Presenter(const Device& device, VkInstance instance, SDL_Window* window);
        ~Presenter();

        Presenter(const Presenter&) = delete;
        Presenter& operator=(const Presenter&) = delete;

        /// Blits `frame` onto the next swapchain image and queues it.
        ///
        /// False where the surface no longer matches the window — a resize, a monitor change or a
        /// compositor restart, none of which is an error. The caller resizes and asks again.
        ///
        /// @param frame must be in `VK_IMAGE_LAYOUT_GENERAL` and hold the picture to show. It is
        ///        left in that layout, which is where the next frame's passes expect it.
        bool present(const Image& frame);

        /// Waits until the present that last read `frame` has finished with it.
        ///
        /// **A present's blit outlives the call that queued it.** `present` submits and returns; the
        /// blit itself waits the acquire semaphore, which under FIFO is not signalled until the
        /// presentation engine has let that swapchain image go. No barrier's source scope reaches
        /// across a submit, so a renderer about to discard and rewrite an image it presented earlier
        /// has to be told when that blit finished, and this is the only thing that can say.
        ///
        /// A no-op for an image this has never presented, and for one whose fence has since been
        /// remade — a swapchain rebuild waits the device idle first, so what it forgets was already
        /// finished.
        void waitForLastUse(const Image& frame);

        /// Rebuilds at `extent`. Waits for everything in flight, so it is a stall by construction.
        void resize(VkExtent2D extent);

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
            VkSemaphore mSemaphore = VK_NULL_HANDLE;

            /// Signalled once the blit that took this slot has run. Null until something takes it.
            VkFence mBlit = VK_NULL_HANDLE;
        };

        /// One per swapchain image, **taken in turn and never indexed by the image**, which is the
        /// one thing an acquire cannot be keyed on: the image is what it returns.
        ///
        /// **A semaphore handed to `vkAcquireNextImageKHR` must carry no operation still pending**,
        /// and the acquire's own signal stays pending until the blit that waits it has run. The blit
        /// is submitted at once but does not run at once — with two frames in flight it queues
        /// behind a whole frame of tracing — so one semaphore for every acquire is a frame handing
        /// the layers `VUID-vkAcquireNextImageKHR-semaphore-01779` and the device an undefined wait.
        /// A slot comes free exactly when its blit's fence signals, which is why the fence is kept
        /// beside it.
        std::vector<Acquisition> mAcquiring;

        /// Which slot the next acquire takes.
        std::uint32_t mAcquisition = 0;

        /// Signalled by the blit and waited by the present. **Per swapchain image and not one**: a
        /// present may still be reading the semaphore a frame signalled, and there is no fence that
        /// says when it stopped.
        std::vector<VkSemaphore> mRendered;

        /// What the last blit onto each image signalled, so one is not written again while its
        /// present is still outstanding.
        std::vector<VkFence> mPresenting;

        std::vector<VkCommandBuffer> mCommands;

        /// Which present a frame image was last read by. One entry per image the renderer alternates
        /// between, so a linear scan is the whole lookup.
        struct ImageUse
        {
            VkImage mImage = VK_NULL_HANDLE;
            VkFence mFence = VK_NULL_HANDLE;
        };
        std::vector<ImageUse> mLastUse;

        /// Whether the surface stopped matching the window since the last rebuild.
        ///
        /// **Kept, because "the window changed size" and "the swapchain went stale" are different
        /// questions with the same answer.** An acquire or a present can fail at a size nothing
        /// asked to change, and a resize that only rebuilt when the extent differed would leave that
        /// one unrecoverable — while rebuilding unconditionally makes the no-op resize a compositor
        /// sends on first map cost a full teardown.
        bool mStale = false;

        /// Its own, because these are re-recorded every frame and the renderer's pool is shaped for
        /// setup work that submits and waits.
        std::unique_ptr<CommandPool> mPool;
    };
}

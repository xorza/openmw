#include "presenter.hpp"

#include <span>
#include <string>
#include <vector>

#include <SDL_error.h>
#include <SDL_stdinc.h>
#include <SDL_video.h>
#include <SDL_vulkan.h>

#include <components/rtx/error.hpp>

#include "barriers.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "image.hpp"
#include "imageuse.hpp"
#include "result.hpp"
#include "swapchain.hpp"
#include "timeline.hpp"

namespace Rtx
{
    namespace
    {
        /// The window's size in pixels, which is not the size it was asked for on a scaled display.
        VkExtent2D drawableSize(SDL_Window* window)
        {
            int width = 0;
            int height = 0;
            SDL_Vulkan_GetDrawableSize(window, &width, &height);
            return VkExtent2D{ static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) };
        }
    }

    std::vector<const char*> Presenter::getInstanceExtensions(SDL_Window* window)
    {
        unsigned int count = 0;
        if (SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr) == SDL_FALSE)
            throw Unsupported(std::string("SDL would not count this window's instance extensions: ") + SDL_GetError());

        std::vector<const char*> names(count);
        if (SDL_Vulkan_GetInstanceExtensions(window, &count, names.data()) == SDL_FALSE)
            throw Unsupported(std::string("SDL would not name this window's instance extensions: ") + SDL_GetError());

        return names;
    }

    Presenter::Presenter(
        const Device& device, VkInstance instance, SDL_Window* window, const SDLUtil::VSyncMode verticalSync)
        : mDevice(device)
        , mInstance(instance)
    {
        try
        {
            if (SDL_Vulkan_CreateSurface(window, instance, &mSurface) == SDL_FALSE)
                throw Unsupported(std::string("SDL would not make a Vulkan surface: ") + SDL_GetError());

            mSwapchain = std::make_unique<Swapchain>(device, mSurface, drawableSize(window), verticalSync);
            remakeImageSync();
        }
        catch (...)
        {
            // A constructor that throws gets no destructor, and the surface is the instance's to
            // free whether or not the swapchain on it was ever built.
            destroy();
            throw;
        }
    }

    Presenter::~Presenter()
    {
        destroy();
    }

    void Presenter::destroy()
    {
        // Through `tearDown`, because this runs from a destructor and from the `catch` that tidies
        // up after a constructor that failed. Throwing out of either is `std::terminate`. This
        // called `vkDeviceWaitIdle` itself to dodge that, which also threw away what the device said
        // about the fault — and left the rule as a comment for the next teardown to remember.
        tearDown("the device would not finish before the presenter was taken apart", [&] { mDevice.waitIdle(); });

        releaseImageSync();

        // After the swapchain, which was made from it.
        mSwapchain.reset();

        if (mSurface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
        mSurface = VK_NULL_HANDLE;
    }

    void Presenter::releaseImageSync()
    {
        // Waited before the semaphores they guard go. A present holds its wait semaphore until
        // the presentation engine is done, and only these say when that is: the device-idle the
        // caller owes proves the queue is empty and nothing more.
        if (mDevice.hasPresentFences())
            for (const SwapImage& image : mImages)
                awaitVk(mDevice, image.mPresented.get(), "the presentation engine letting go of an image");

        // Freed and not merely reset: a recording that blitted from the renderer's target still
        // names it, and a rebuild that allocated a fresh set would leave the old one in the pool
        // for the presenter's life — a window resized or a vsync changed a few dozen times is a
        // few dozen sets.
        std::vector<VkCommandBuffer> commands;
        commands.reserve(mImages.size());
        for (const SwapImage& image : mImages)
            commands.push_back(image.mCommands);
        mDevice.getPool().free(commands);

        mAcquiring.clear();
        mImages.clear();
    }

    void Presenter::remakeImageSync()
    {
        releaseImageSync();

        const std::uint32_t images = mSwapchain->getImageCount();

        // Made again rather than reused, because a slot can arrive here signalled with nothing
        // left to wait it: a suboptimal acquire hands back both an image and a signal, and it is the
        // present after it that reports the swapchain stale. Destroying the semaphore is what clears
        // that signal, and `releaseImageSync` above is where it happens.
        mAcquiring.resize(images);
        for (Acquisition& acquisition : mAcquiring)
            acquisition.mSemaphore = makeSemaphore(mDevice);
        mAcquisition = 0;

        // A blit stamp of nought, which the timeline has passed: no image has been blitted onto
        // yet.
        const std::vector<VkCommandBuffer> commands = mDevice.getPool().allocate(images);
        mImages.resize(images);
        for (std::uint32_t index = 0; index < images; ++index)
        {
            SwapImage& image = mImages[index];
            image.mRendered = makeSemaphore(mDevice);
            image.mBlitOn = 0;
            if (mDevice.hasPresentFences())
                image.mPresented = makeSignalledFence(mDevice);
            image.mCommands = commands[index];
        }

        // The blits those entries name have run, and forgetting is the whole of what is owed: the
        // device was waited idle to get here. It is also what keeps an entry from meeting a new
        // image on a recycled handle — a renderer resizes its targets through this, and always
        // after this.
        mLastUse.clear();
    }

    bool Presenter::wantsResize(const VkExtent2D extent)
    {
        if (!mStale && extent.width == getExtent().width && extent.height == getExtent().height)
            return false;

        // A window that is not on screen is left alone. Its surface reports no extent, a
        // swapchain of none is invalid usage, and rebuilding once a frame against a surface that
        // will not take one is a rebuild a minimised game would pay for as long as it stayed
        // minimised. The staleness stands, so the window coming back rebuilds then.
        if (mSwapchain->surfaceIsHidden())
        {
            mStale = true;
            return false;
        }

        return true;
    }

    void Presenter::rebuild(const VkExtent2D extent)
    {
        mDevice.waitIdle();
        mSwapchain->recreate(extent);
        remakeImageSync();
        mStale = false;
    }

    void Presenter::setVerticalSync(SDLUtil::VSyncMode mode)
    {
        if (!mSwapchain->setVerticalSync(mode))
            return;

        // The same three calls `rebuild` makes, because a present mode is a property of the
        // swapchain object. Not `rebuild` itself, because that clears a staleness a window that
        // changed size meanwhile still owes.
        mDevice.waitIdle();
        mSwapchain->recreate(getExtent());
        remakeImageSync();
    }

    VkExtent2D Presenter::getExtent() const
    {
        return mSwapchain->getExtent();
    }

    bool Presenter::present(const Image& frame)
    {
        const Timeline& timeline = mDevice.getTimeline();

        Acquisition& acquisition = mAcquiring[mAcquisition];
        mAcquisition = (mAcquisition + 1) % static_cast<std::uint32_t>(mAcquiring.size());

        // A slot is free when its blit has run, and not when the call that queued it returned.
        // The blit waits the semaphore the acquire signalled, so until it runs both operations are
        // still pending on that semaphore and it may not be handed to another acquire.
        timeline.waitFor(acquisition.mBlit, "the blit that last took this acquire semaphore");

        std::uint32_t index = 0;
        if (!mSwapchain->acquire(acquisition.mSemaphore.get(), index))
        {
            mStale = true;
            return false;
        }

        // This image may still be in the presentation engine's hands. Mailbox releases a frame
        // the moment a newer one replaces it, so an image can come back round before the present
        // that queued it has consumed its semaphore — the case a count of frames in flight does not
        // cover, because it counts frames rather than images.
        SwapImage& image = mImages[index];
        timeline.waitFor(image.mBlitOn, "the blit that last wrote this image");

        // And the present itself, which is a different moment: the blit's value says the queue has
        // run the copy, and this says the compositor has let go of what it copied into. Without it
        // the semaphore below is signalled again while a present still waits on it.
        if (mDevice.hasPresentFences())
        {
            const VkFence presented = image.mPresented.get();
            awaitVk(mDevice, presented, "the presentation engine letting go of this image");
            checkVk(vkResetFences(mDevice.getHandle(), 1, &presented), "vkResetFences");
        }

        const VkCommandBuffer commands = image.mCommands;
        mDevice.getPool().begin(commands);

        frame.transition(commands, Use::sAnyGeneralWrite, Use::sBlitRead);

        // The source scope names the stage the acquire semaphore is waited at, or the transition
        // is ordered against nothing and can run before the image is ours. `TOP_OF_PIPE` as a source
        // scope means exactly that: nothing.
        const VkImage presented = mSwapchain->getImage(index);
        Barriers taken(commands);
        taken.add(imageBarrier(
            presented, 0, 1, ImageUse{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_BLIT_BIT, 0 }, Use::sBlitWrite));
        taken.flush();

        const VkExtent2D extent = mSwapchain->getExtent();
        const VkImageBlit region{
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets
            = { {}, { static_cast<std::int32_t>(frame.getWidth()), static_cast<std::int32_t>(frame.getHeight()), 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets
            = { {}, { static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height), 1 } },
        };
        vkCmdBlitImage(commands, frame.getHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, presented,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);

        Barriers handed(commands);
        handed.add(imageBarrier(presented, 0, 1, Use::sBlitWrite, Use::sPresent));

        // Back where the next frame's passes expect to find it.
        handed.add(frame.describeTransition(Use::sBlitRead, Use::sAnyGeneralWrite));
        handed.flush();

        // The pool's submit, so it signals the timeline and carries what was deferred ahead of the
        // blit — and waits the acquire and signals the present beside that.
        const VkSemaphoreSubmitInfo wait{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = acquisition.mSemaphore.get(),
            .stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
        };
        const VkSemaphoreSubmitInfo signal{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = image.mRendered.get(),
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        const std::uint64_t blitted = mDevice.getPool().submit(commands,
            std::span<const VkSemaphoreSubmitInfo>(&wait, 1), std::span<const VkSemaphoreSubmitInfo>(&signal, 1));

        acquisition.mBlit = blitted;
        image.mBlitOn = blitted;
        rememberUse(frame.getHandle(), blitted);

        if (mSwapchain->present(image.mRendered.get(), index, image.mPresented.get()))
            return true;

        mStale = true;
        return false;
    }

    void Presenter::rememberUse(VkImage image, std::uint64_t value)
    {
        for (LastUse& use : mLastUse)
            if (use.mImage == image)
            {
                use.mBlit = value;
                return;
            }

        mLastUse.push_back(LastUse{ .mImage = image, .mBlit = value });
    }

    void Presenter::waitForLastUse(const Image& frame)
    {
        for (const LastUse& use : mLastUse)
            if (use.mImage == frame.getHandle())
            {
                mDevice.getTimeline().waitFor(use.mBlit, "the blit that last read this frame");
                return;
            }
    }
}

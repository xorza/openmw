#include "presenter.hpp"

#include <SDL_vulkan.h>

#include <components/rtx/error.hpp>

#include "commands.hpp"
#include "device.hpp"
#include "image.hpp"
#include "result.hpp"
#include "swapchain.hpp"

namespace Rtx
{
    namespace
    {
        VkSemaphore makeSemaphore(VkDevice device)
        {
            const VkSemaphoreCreateInfo create{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            VkSemaphore semaphore = VK_NULL_HANDLE;
            checkVk(vkCreateSemaphore(device, &create, nullptr, &semaphore), "vkCreateSemaphore");
            return semaphore;
        }

        VkFence makeSignalledFence(VkDevice device)
        {
            const VkFenceCreateInfo create{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
            };
            VkFence fence = VK_NULL_HANDLE;
            checkVk(vkCreateFence(device, &create, nullptr, &fence), "vkCreateFence");
            return fence;
        }

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
            throw Error(std::string("SDL would not count this window's instance extensions: ") + SDL_GetError());

        std::vector<const char*> names(count);
        if (SDL_Vulkan_GetInstanceExtensions(window, &count, names.data()) == SDL_FALSE)
            throw Error(std::string("SDL would not name this window's instance extensions: ") + SDL_GetError());

        return names;
    }

    Presenter::Presenter(const Device& device, VkInstance instance, SDL_Window* window)
        : mDevice(device)
        , mInstance(instance)
    {
        try
        {
            if (SDL_Vulkan_CreateSurface(window, instance, &mSurface) == SDL_FALSE)
                throw Error(std::string("SDL would not make a Vulkan surface: ") + SDL_GetError());

            mSwapchain = std::make_unique<Swapchain>(device, mSurface, drawableSize(window));
            mPool = std::make_unique<CommandPool>(device);
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
        // **`vkDeviceWaitIdle` and not `Device::waitIdle`.** The wrapper reports a failure by
        // throwing, and this runs from a destructor where throwing is a call to `std::terminate`.
        // Nothing here could be done about a device that will not go idle anyway.
        vkDeviceWaitIdle(mDevice.getHandle());

        releaseImageSync();
        mPool.reset();

        // After the swapchain, which was made from it.
        mSwapchain.reset();

        if (mSurface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
        mSurface = VK_NULL_HANDLE;
    }

    void Presenter::releaseImageSync()
    {
        for (const Acquisition& acquisition : mAcquiring)
            vkDestroySemaphore(mDevice.getHandle(), acquisition.mSemaphore, nullptr);
        mAcquiring.clear();

        for (const VkSemaphore semaphore : mRendered)
            vkDestroySemaphore(mDevice.getHandle(), semaphore, nullptr);
        mRendered.clear();

        for (const VkFence fence : mPresenting)
            vkDestroyFence(mDevice.getHandle(), fence, nullptr);
        mPresenting.clear();
    }

    void Presenter::remakeImageSync()
    {
        releaseImageSync();

        // **Rebuilt with the swapchain, because the image count is the surface's to decide.** A
        // recreate can come back with a different number, and a vector sized to the old one is then
        // indexed past its end — which hands `vkQueueSubmit2` a semaphore made of whatever was next
        // on the heap. The layers say so at once; with them off it is a frozen window and an empty
        // log.
        // **Before the buffers are handed out again**, and the reason is not tidiness: a recording
        // that blitted from the renderer's target still names it, and the caller is about to destroy
        // that target and make a new one at the new size.
        mPool->reset();

        const std::uint32_t images = mSwapchain->getImageCount();

        // **Made again rather than reused**, because a slot can arrive here signalled with nothing
        // left to wait it: a suboptimal acquire hands back both an image and a signal, and it is the
        // present after it that reports the swapchain stale. Destroying the semaphore is what clears
        // that signal, and `releaseImageSync` above is where it happens.
        mAcquiring.assign(images, Acquisition{});
        for (Acquisition& acquisition : mAcquiring)
            acquisition.mSemaphore = makeSemaphore(mDevice.getHandle());
        mAcquisition = 0;

        mRendered.assign(images, VK_NULL_HANDLE);
        for (VkSemaphore& semaphore : mRendered)
            semaphore = makeSemaphore(mDevice.getHandle());

        mPresenting.assign(images, VK_NULL_HANDLE);
        for (VkFence& fence : mPresenting)
            fence = makeSignalledFence(mDevice.getHandle());

        // The fences those entries name have just been destroyed, and forgetting is the whole of
        // what is owed: the device was waited idle to get here, so every one of them had signalled.
        // It is also what keeps an entry from meeting a new image on a recycled handle — a renderer
        // resizes its targets through this, and always after this.
        mLastUse.clear();

        mCommands = mPool->allocate(images);
    }

    void Presenter::resize(VkExtent2D extent)
    {
        if (!mStale && extent.width == getExtent().width && extent.height == getExtent().height)
            return;

        mDevice.waitIdle();
        mSwapchain->recreate(extent);
        remakeImageSync();
        mStale = false;
    }

    void Presenter::setVerticalSync(SDLUtil::VSyncMode mode)
    {
        if (!mSwapchain->setVerticalSync(mode))
            return;

        // The same rebuild `resize` makes, for the same reason: a present mode is a property of the
        // swapchain object, so changing it means a new one and every frame in flight has to be done
        // with the old one first.
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
        Acquisition& acquisition = mAcquiring[mAcquisition];
        mAcquisition = (mAcquisition + 1) % static_cast<std::uint32_t>(mAcquiring.size());

        // **A slot is free when its blit has run, and not when the call that queued it returned.**
        // The blit waits the semaphore the acquire signalled, so until it runs both operations are
        // still pending on that semaphore and it may not be handed to another acquire.
        if (acquisition.mBlit != VK_NULL_HANDLE)
            awaitVk(mDevice, acquisition.mBlit, "the blit that last took this acquire semaphore");
        acquisition.mBlit = VK_NULL_HANDLE;

        std::uint32_t index = 0;
        if (!mSwapchain->acquire(acquisition.mSemaphore, index))
        {
            mStale = true;
            return false;
        }

        // **This image may still be in the presentation engine's hands.** Mailbox releases a frame
        // the moment a newer one replaces it, so an image can come back round before the present
        // that queued it has consumed its semaphore — the case a count of frames in flight does not
        // cover, because it counts frames rather than images.
        awaitVk(mDevice, mPresenting[index], "the present that last used this image");
        checkVk(vkResetFences(mDevice.getHandle(), 1, &mPresenting[index]), "vkResetFences");

        const VkCommandBuffer commands = mCommands[index];
        const VkCommandBufferBeginInfo begin{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        checkVk(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");

        frame.transition(commands, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);

        const VkImage presented = mSwapchain->getImage(index);

        // **The source scope names the stage the acquire semaphore is waited at**, or the transition
        // is ordered against nothing and can run before the image is ours. `TOP_OF_PIPE` as a source
        // scope means exactly that: nothing.
        VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = presented,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };
        vkCmdPipelineBarrier2(commands, &dependency);

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

        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier2(commands, &dependency);

        // Back where the next frame's passes expect to find it.
        frame.transition(commands, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT);

        checkVk(vkEndCommandBuffer(commands), "vkEndCommandBuffer");

        const VkSemaphoreSubmitInfo wait{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = acquisition.mSemaphore,
            .stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
        };
        const VkSemaphoreSubmitInfo signal{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = mRendered[index],
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
        const VkCommandBufferSubmitInfo buffer{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = commands,
        };
        const VkSubmitInfo2 submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = 1,
            .pWaitSemaphoreInfos = &wait,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &buffer,
            .signalSemaphoreInfoCount = 1,
            .pSignalSemaphoreInfos = &signal,
        };
        checkVk(mDevice, vkQueueSubmit2(mDevice.getQueue(), 1, &submit, mPresenting[index]), "vkQueueSubmit2");

        acquisition.mBlit = mPresenting[index];
        rememberUse(frame.getHandle(), mPresenting[index]);

        if (mSwapchain->present(mRendered[index], index))
            return true;

        mStale = true;
        return false;
    }

    void Presenter::rememberUse(VkImage image, VkFence fence)
    {
        for (ImageUse& use : mLastUse)
            if (use.mImage == image)
            {
                use.mFence = fence;
                return;
            }

        mLastUse.push_back(ImageUse{ .mImage = image, .mFence = fence });
    }

    void Presenter::waitForLastUse(const Image& frame)
    {
        for (const ImageUse& use : mLastUse)
            if (use.mImage == frame.getHandle())
            {
                // The fence may have been reset and signalled again by a later present of another
                // image. That is conservative and not wrong: a queue signals its fences in submission
                // order, so the later one having finished means this one had.
                awaitVk(mDevice, use.mFence, "the present that last read this frame");
                return;
            }
    }
}

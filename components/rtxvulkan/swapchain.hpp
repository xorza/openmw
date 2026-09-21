#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/sdlutil/vsyncmode.hpp>

#include "owned.hpp"
#include "pacedmodes.hpp"

namespace Rtx
{
    class Device;

    /// The images the window presents, and the two calls that hand them back and forth. The
    /// renderer renders into an image of its own and blits, because the format a surface offers is
    /// not one a compute shader may store to.
    class Swapchain
    {
    public:
        /// @param preferPaced whether the player asked for the driver's pacing, which is what
        ///        puts a paced mode ahead of an unpaced one where the vertical sync leaves the
        ///        choice — `presentModeFor`.
        /// @param paced which present modes the surface paces under, the presenter's for as long
        ///        as this lives: a swapchain made under one of them is opted into the driver's
        ///        pacing, and `isPaced` says so for the pacer to follow.
        Swapchain(const Device& device, VkSurfaceKHR surface, VkExtent2D extent, SDLUtil::VSyncMode verticalSync,
            bool preferPaced, const PacedModes& paced);

        /// Takes the next image. False means the swapchain no longer matches the window and must be
        /// recreated — which a resize, a monitor change or a compositor restart all cause, and none
        /// of which is an error.
        bool acquire(VkSemaphore ready, std::uint32_t& index);

        /// Hands the image back. False means the same thing as it does for `acquire`.
        /// @param presented signalled when the presentation engine has finished with the image, or
        ///        null where the device offers no such fence. `Presenter::mPresented` says why one
        ///        is wanted.
        /// @param presentId what the driver's pacing knows this frame as, or nought for a present
        ///        nothing paces, which carries no id.
        bool present(VkSemaphore finished, std::uint32_t index, VkFence presented, std::uint64_t presentId);

        /// Rebuilds at a new size. The caller must have waited for every frame still in flight.
        void recreate(VkExtent2D extent);

        /// Whether the surface has no extent at all, which is what a minimised window reports.
        /// Asked of the surface and not of this, because what this holds is the size it was last
        /// built at: a window minimised after that still reports its old extent here and none there.
        bool surfaceIsHidden() const;

        /// Says how the presented image should meet the refresh, and answers whether that changed
        /// the present mode, which is what says a rebuild is owed. A setting and not a mode,
        /// because two settings collapse onto one mode where a driver is missing the other.
        bool setVerticalSync(SDLUtil::VSyncMode mode);

        /// Says whether the player asked for the driver's pacing, and answers the same way: the
        /// vertical sync's `Disabled` is immediate where the driver paces it and mailbox where not.
        bool setPreferPaced(bool preferPaced);

        VkExtent2D getExtent() const { return mExtent; }
        VkImage getImage(std::uint32_t index) const { return mImages[index]; }
        std::uint32_t getImageCount() const { return static_cast<std::uint32_t>(mImages.size()); }
        VkSwapchainKHR getHandle() const { return mHandle.get(); }

        /// Whether this swapchain was made opted into the driver's pacing: the surface paces the
        /// mode it presents in. Decided per creation, so a mode change decides again.
        bool isPaced() const { return mPaced; }

    private:
        void create(VkExtent2D extent);
        void destroy();

        /// Picks the mode the two settings ask for, and says whether it moved.
        bool chooseMode();

        const Device& mDevice;
        VkSurfaceKHR mSurface = VK_NULL_HANDLE;
        Owned<VkSwapchainKHR, vkDestroySwapchainKHR> mHandle;
        VkSurfaceFormatKHR mFormat{};
        VkPresentModeKHR mPresentMode = VK_PRESENT_MODE_FIFO_KHR;

        SDLUtil::VSyncMode mVerticalSync;
        VkExtent2D mExtent{};
        std::vector<VkImage> mImages;

        const PacedModes& mPacedModes;
        bool mPreferPaced = false;
        bool mPaced = false;
    };
}

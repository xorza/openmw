#pragma once

#include <utility>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// A Vulkan handle the device destroys, and the device it belongs to: the one place
    /// `vkDestroyX(device, handle, allocator)` is spelled, so the classes holding one default their
    /// moves. `CI/check_rtx_handles.sh` is what makes the next class adopt it, and names the calls
    /// this shape cannot take.
    ///
    /// @tparam Destroy the function that ends it.
    template <class Handle, auto Destroy>
    class Owned
    {
    public:
        Owned() = default;

        Owned(VkDevice device, Handle handle)
            : mDevice(device)
            , mHandle(handle)
        {
        }

        ~Owned() { reset(); }

        Owned(const Owned&) = delete;
        Owned& operator=(const Owned&) = delete;

        Owned(Owned&& other) noexcept
            : mDevice(other.mDevice)
            , mHandle(std::exchange(other.mHandle, VK_NULL_HANDLE))
        {
        }

        Owned& operator=(Owned&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                mDevice = other.mDevice;
                mHandle = std::exchange(other.mHandle, VK_NULL_HANDLE);
            }

            return *this;
        }

        Handle get() const { return mHandle; }

        /// The device it belongs to, for a call that needs both.
        VkDevice getDevice() const { return mDevice; }

        /// Where to put one, for a call that fills a handle in rather than returning it.
        Handle* put(VkDevice device)
        {
            reset();
            mDevice = device;
            return &mHandle;
        }

        void reset()
        {
            if (mHandle != VK_NULL_HANDLE)
                Destroy(mDevice, mHandle, nullptr);

            mHandle = VK_NULL_HANDLE;
        }

        /// Hands the handle over undestroyed, for a caller that buries it instead: a batch in
        /// flight may still name it.
        Handle release() { return std::exchange(mHandle, VK_NULL_HANDLE); }

    private:
        VkDevice mDevice = VK_NULL_HANDLE;
        Handle mHandle = VK_NULL_HANDLE;
    };
}

#pragma once

#include <utility>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// A Vulkan handle the device destroys, and the device it belongs to.
    ///
    /// **The one place `vkDestroyX(device, handle, allocator)` is spelled.** Every class that holds
    /// one used to write the same three things by hand — a move constructor, a move assignment and a
    /// `destroy` — which is twenty lines with one name changed in them, and a class that forgot the
    /// exchange in its move leaked whatever the source still held. A member that empties itself is
    /// what lets those classes default their moves and say nothing at all.
    ///
    /// @tparam Destroy the function that ends it, which every one of these spells the same way.
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

    private:
        VkDevice mDevice = VK_NULL_HANDLE;
        Handle mHandle = VK_NULL_HANDLE;
    };
}

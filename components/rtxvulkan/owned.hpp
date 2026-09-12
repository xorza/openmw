#pragma once

#include <utility>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// A Vulkan handle the device destroys, and the device it belongs to.
    ///
    /// **The one place `vkDestroyX(device, handle, allocator)` is spelled.** A class that holds one
    /// and writes a move constructor, a move assignment and a `destroy` by hand is twenty lines
    /// with one name changed in them, and one that forgets the exchange in its move leaks whatever
    /// the source still held. A member that empties itself is what lets those classes default
    /// their moves and say nothing at all.
    ///
    /// **`CI/check_rtx_handles.sh` is what makes the next class adopt it**, because saying so here
    /// did not: a class that spelled the call itself cost a destructor and a `const Device&` member
    /// that existed only so that destructor could reach the device, and nothing asked. The script
    /// names the calls this shape cannot take, and why each cannot.
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

        /// Hands the handle over undestroyed, for a caller that buries it instead: a batch in
        /// flight may still name it.
        Handle release() { return std::exchange(mHandle, VK_NULL_HANDLE); }

    private:
        VkDevice mDevice = VK_NULL_HANDLE;
        Handle mHandle = VK_NULL_HANDLE;
    };
}

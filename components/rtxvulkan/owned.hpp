#pragma once

#include <utility>

#include <vulkan/vulkan_core.h>

#include "result.hpp"

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

        VkDevice getDevice() const { return mDevice; }

        /// Where to put one, for a call that fills a handle in rather than returning it.
        Handle* put(VkDevice device)
        {
            reset();
            mDevice = device;
            return &mHandle;
        }

        /// One made by `create(device, &info, allocator, out)`, which is the shape every
        /// `vkCreateX` this backend calls has but the pipelines' — checked, and named by `call`
        /// in the message a failure carries. The string stays: nothing in C++ names a function
        /// pointer's function.
        template <class Create, class Info>
        static Owned make(VkDevice device, Create create, const Info& info, const char* call)
        {
            Owned made;
            checkVk(create(device, &info, nullptr, made.put(device)), call);
            return made;
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

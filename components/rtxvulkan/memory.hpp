#pragma once

#include <cstddef>

#include <vulkan/vulkan_core.h>

#include <components/rtx/memoryreport.hpp>

struct VmaAllocation_T;
struct VmaAllocator_T;

namespace Rtx
{
    class MemoryAllocator;

    /// `value` rounded up to the next multiple of `alignment`.
    inline VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    /// Video memory the host writes into and the device reads — one statement, because
    /// `Buffer::hostWritten` asks for it, `MemoryAllocator::report` counts it, and a device
    /// offering no such type is refused.
    inline constexpr VkMemoryPropertyFlags sHostWritten = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    /// A range of one device allocation, and the allocator that hands it back. Not an allocation
    /// of its own: one `vkAllocateMemory` per image was 1554 for the cell the game starts in, most
    /// for a shading map of two kilobytes; sub-allocated, that cell takes a couple of dozen.
    class DeviceMemory
    {
    public:
        DeviceMemory() = default;
        ~DeviceMemory();

        DeviceMemory(const DeviceMemory&) = delete;
        DeviceMemory& operator=(const DeviceMemory&) = delete;

        /// Written out, because a moved-from range must stop naming the allocation. Everything else
        /// here empties itself; a range the source still described would be given back twice.
        DeviceMemory(DeviceMemory&& other) noexcept;
        DeviceMemory& operator=(DeviceMemory&& other) noexcept;

        VkDeviceMemory getHandle() const { return mHandle; }

        VkDeviceSize getOffset() const { return mOffset; }

        /// This range, mapped, or null where the memory is not host-visible. Mapped for as long as
        /// the range stands, and memory need not be unmapped before it is freed.
        void* map() const { return mMapped; }

    private:
        friend class MemoryAllocator;

        DeviceMemory(VmaAllocator_T* owner, VmaAllocation_T* allocation, VkDeviceMemory handle, VkDeviceSize offset,
            void* mapped);

        VmaAllocator_T* mOwner = nullptr;
        VmaAllocation_T* mAllocation = nullptr;
        VkDeviceMemory mHandle = VK_NULL_HANDLE;
        VkDeviceSize mOffset = 0;
        void* mMapped = nullptr;
    };

    /// Every `vkAllocateMemory` the renderer holds, and the ranges of them nothing is using:
    /// Vulkan Memory Allocator behind a face of this fork's own, so that a buffer and an image ask
    /// for room the same way and a report reads one set of figures. Every allocation carries the
    /// device-address flag, because every buffer here may be addressed; a host-visible range is
    /// mapped as it is made. Thread-safe, because `VisibilityPass::compileEvery` reaches `take`
    /// from a thread per core.
    class MemoryAllocator
    {
    public:
        /// @param memory the device's heaps and types, read once when the device was chosen. Kept
        ///        for the report, which says what each heap is beside what it holds.
        /// @param budget whether `VK_EXT_memory_budget` was enabled. Optional, because a driver
        ///        without it only stops the renderer saying how close to the ceiling it is.
        MemoryAllocator(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
            const VkPhysicalDeviceMemoryProperties& memory, bool budget);
        ~MemoryAllocator();

        MemoryAllocator(const MemoryAllocator&) = delete;
        MemoryAllocator& operator=(const MemoryAllocator&) = delete;

        /// Room for `buffer`, in memory that is `properties`, ready to bind, at the coarser of the
        /// driver's alignment for binding it and `alignment` — what the buffer's use owes beyond the
        /// bind, which `Buffer` reads off its usage. Throws `Unsupported` where the device offers
        /// no memory type that is `properties`, which on hardware that meets the requirements means
        /// the request was wrong.
        DeviceMemory take(VkBuffer buffer, VkMemoryPropertyFlags properties, VkDeviceSize alignment);

        /// The same for an image, at the driver's alignment, which the allocator keeps its
        /// granularity away from a buffer's.
        DeviceMemory take(VkImage image, VkMemoryPropertyFlags properties);

        /// How many calls to `vkAllocateMemory` stand behind everything handed out.
        std::size_t getBlockCount() const;

        /// How many ranges stand, which is what a device may not be taken apart under.
        std::size_t getLiveCount() const;

        /// Every heap of the device, what this allocator took out of each, and what the driver says
        /// is left. Walks every allocation for the host-written figures, so it is asked once a place
        /// and never once a frame.
        MemoryReport report() const;

    private:
        VkDevice mDevice = VK_NULL_HANDLE;
        const VkPhysicalDeviceMemoryProperties& mMemory;

        /// Whether `report` may ask the driver what this process holds, rather than only counting
        /// what it asked for itself.
        bool mBudget = false;

        VmaAllocator_T* mAllocator = nullptr;
    };
}

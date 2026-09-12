#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/memoryreport.hpp>
#include <components/rtx/runs.hpp>

#include "owned.hpp"

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

    /// Which side of `bufferImageGranularity` a resource sits on. `MemoryAllocator` keeps a pool
    /// apiece, so a buffer and an image never share an allocation and the rule is settled once.
    enum class Tiling
    {
        Linear,
        Optimal,
    };

    /// A range of one device allocation, and the allocator that hands it back. Not an allocation
    /// of its own: one `vkAllocateMemory` per image was 1554 for the cell the game starts in, most
    /// for a shading map of two kilobytes; `MemoryAllocator` makes 24 for that cell.
    class DeviceMemory
    {
    public:
        DeviceMemory() = default;
        ~DeviceMemory();

        DeviceMemory(const DeviceMemory&) = delete;
        DeviceMemory& operator=(const DeviceMemory&) = delete;

        /// Written out, because a moved-from range must stop naming the block. Everything else
        /// here empties itself; a range the source still described would be given back twice.
        DeviceMemory(DeviceMemory&& other) noexcept;
        DeviceMemory& operator=(DeviceMemory&& other) noexcept;

        VkDeviceMemory getHandle() const { return mHandle; }

        VkDeviceSize getOffset() const { return mOffset; }

        /// This range, mapped, or null where the memory is not host-visible. The block is mapped
        /// once at allocation and never unmapped, and memory need not be unmapped before it is
        /// freed.
        void* map() const { return mMapped; }

    private:
        friend class MemoryAllocator;

        DeviceMemory(MemoryAllocator& owner, std::uint32_t block, Run run, VkDeviceMemory handle, VkDeviceSize offset,
            void* mapped);

        MemoryAllocator* mOwner = nullptr;
        VkDeviceMemory mHandle = VK_NULL_HANDLE;
        VkDeviceSize mOffset = 0;
        void* mMapped = nullptr;
        Run mRun;
        std::uint32_t mBlock = 0;
    };

    /// Every `vkAllocateMemory` the renderer holds, and the ranges of them nothing is using. The
    /// same shape as `StructureStorage`, over device memory: a block is made once, a `RunAllocator`
    /// says where inside it a resource goes, and a range given back is merged with what it touches,
    /// so a cell that leaves hands its textures' memory to the cell that arrives. A pool per memory
    /// type and per tiling, which keeps a buffer and an image off the same
    /// `bufferImageGranularity` page. Locked, because `VisibilityPass::compileEvery` reaches `take`
    /// from a thread per core; uncontended on the frame path.
    class MemoryAllocator
    {
    public:
        /// What a range's offset and length are counted in: 1024 bytes, the coarsest alignment any
        /// image or buffer this renderer creates asks for on this hardware. One that asks for more
        /// takes as many extra pages as its alignment exceeds one by.
        static constexpr VkDeviceSize sPage = 1024;

        /// @param physicalDevice the card, kept only so that the budget below can be asked of it.
        /// @param memory the device's heaps and types, read once when the device was chosen.
        /// @param budget whether `VK_EXT_memory_budget` was enabled. Optional, because a driver
        ///        without it only stops the renderer saying how close to the ceiling it is.
        MemoryAllocator(VkDevice device, VkPhysicalDevice physicalDevice,
            const VkPhysicalDeviceMemoryProperties& memory, bool budget);
        ~MemoryAllocator();

        /// Room for a resource with `requirements`, in memory that is `properties`. Throws
        /// `Unsupported` where the device offers no memory type that is both, which on hardware
        /// that meets the requirements means the request was wrong.
        DeviceMemory take(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties, Tiling tiling);

        /// How many calls to `vkAllocateMemory` stand behind everything handed out — the
        /// allocations and not the slots, because a block given back to the device leaves its slot
        /// for the next one, since a range names its block by index.
        std::size_t getBlockCount() const;

        /// Every heap of the device, what this allocator took out of each, and what the driver says
        /// is left. Walks every block, so it is asked once a place and never once a frame.
        MemoryReport report() const;

    private:
        friend class DeviceMemory;

        /// One `vkAllocateMemory` and what has been handed out inside it. The allocator has no
        /// block boundary of its own: this allocation *is* the block, and what stops a range
        /// leaving it is `mPages` checked against `getEnd`.
        struct Block
        {
            Owned<VkDeviceMemory, vkFreeMemory> mHandle;
            void* mMapped = nullptr;
            RunAllocator mRuns;
            std::uint32_t mPages = 0;
            std::uint32_t mPool = 0;
        };

        /// The index of a memory type satisfying `properties`, out of those `typeBits` allows.
        std::uint32_t findType(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const;

        /// How large a block of `type` is made, before what a single resource may force.
        VkDeviceSize blockBytes(std::uint32_t type, std::uint32_t held) const;

        /// `run` in block `at`, as the range a resource of `alignment` is bound in.
        DeviceMemory place(std::uint32_t at, Run run, VkDeviceSize alignment);

        /// Gives a range back, and hands the block behind it to the device where that was the last
        /// range in it — at most one `vkFreeMemory` per range, never a sweep. The last block of a
        /// pool is kept, or a pool that emptied and refilled would free and allocate on alternate
        /// frames. Called by `DeviceMemory` and by nothing else.
        void give(std::uint32_t block, Run run);

        /// Held by everything that touches the list below.
        mutable std::mutex mLock;

        VkDevice mDevice = VK_NULL_HANDLE;
        VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
        const VkPhysicalDeviceMemoryProperties& mMemory;

        /// Whether `report` may ask the driver what this process holds, rather than only counting
        /// what it asked for itself.
        bool mBudget = false;

        /// Every block of every pool, in one list. A block's slot is never removed, so the index a
        /// range carries names the same block for the allocator's life; a slot whose allocation
        /// went back to the device is taken over by the next block that pool needs.
        std::vector<Block> mBlocks;

        /// How many blocks of each pool hold an allocation, kept rather than counted, because
        /// `give` runs per resource.
        std::vector<std::uint32_t> mBlocksInPool;
    };
}

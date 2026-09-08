#include "memory.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <utility>

#include <components/rtx/error.hpp>

#include "result.hpp"

namespace Rtx
{
    namespace
    {
        /// The smallest block a pool starts with, and the largest it grows one to.
        ///
        /// **A pool doubles until it reaches the ceiling.** A pool that stands two kilobytes should
        /// not reserve the whole ceiling for them, and a pool that stands a cell's textures — 62 MiB
        /// at Seyda Neen, measured — should reach them in a handful of calls rather than in dozens.
        /// Doubling from the floor is what answers both: 8, 16, 32, then 64 for ever after.
        constexpr VkDeviceSize sSmallestBlock = 8 * 1024 * 1024;
        constexpr VkDeviceSize sLargestBlock = 64 * 1024 * 1024;

        /// The pool a resource of `type` and `tiling` comes out of, and the type it was made from.
        ///
        /// **One number, because a block names its pool and nothing else about it.** Written here
        /// rather than at the two places that pack and unpack it, so a report reading a block's type
        /// back cannot disagree with what `take` put in.
        std::uint32_t poolOf(std::uint32_t type, Tiling tiling)
        {
            return 2 * type + (tiling == Tiling::Linear ? 0u : 1u);
        }

        std::uint32_t typeOf(std::uint32_t pool)
        {
            return pool / 2;
        }

        /// How many pages a resource of `size` needs, given where its `alignment` may push it.
        ///
        /// A page boundary is already a multiple of every alignment up to a page, so only a coarser
        /// one costs anything: the range has to hold the resource wherever inside it the alignment
        /// lands, which is at most a page short of one whole alignment further on.
        std::uint32_t pagesFor(VkDeviceSize size, VkDeviceSize alignment)
        {
            assert(
                alignment > 0 && (alignment & (alignment - 1)) == 0 && "Vulkan states an alignment as a power of two");

            const VkDeviceSize slack = alignment > MemoryAllocator::sPage ? alignment - MemoryAllocator::sPage : 0;

            return static_cast<std::uint32_t>(alignUp(size + slack, MemoryAllocator::sPage) / MemoryAllocator::sPage);
        }
    }

    DeviceMemory::DeviceMemory(
        MemoryAllocator& owner, std::uint32_t block, Run run, VkDeviceMemory handle, VkDeviceSize offset, void* mapped)
        : mOwner(&owner)
        , mHandle(handle)
        , mOffset(offset)
        , mMapped(mapped)
        , mRun(run)
        , mBlock(block)
    {
    }

    DeviceMemory::~DeviceMemory()
    {
        if (mOwner != nullptr)
            mOwner->give(mBlock, mRun);
    }

    DeviceMemory::DeviceMemory(DeviceMemory&& other) noexcept
        : mOwner(std::exchange(other.mOwner, nullptr))
        , mHandle(std::exchange(other.mHandle, VK_NULL_HANDLE))
        , mOffset(std::exchange(other.mOffset, 0))
        , mMapped(std::exchange(other.mMapped, nullptr))
        , mRun(std::exchange(other.mRun, Run{}))
        , mBlock(other.mBlock)
    {
    }

    DeviceMemory& DeviceMemory::operator=(DeviceMemory&& other) noexcept
    {
        if (this != &other)
        {
            if (mOwner != nullptr)
                mOwner->give(mBlock, mRun);

            mOwner = std::exchange(other.mOwner, nullptr);
            mHandle = std::exchange(other.mHandle, VK_NULL_HANDLE);
            mOffset = std::exchange(other.mOffset, 0);
            mMapped = std::exchange(other.mMapped, nullptr);
            mRun = std::exchange(other.mRun, Run{});
            mBlock = other.mBlock;
        }

        return *this;
    }

    MemoryAllocator::MemoryAllocator(
        VkDevice device, VkPhysicalDevice physicalDevice, const VkPhysicalDeviceMemoryProperties& memory, bool budget)
        : mDevice(device)
        , mPhysicalDevice(physicalDevice)
        , mMemory(memory)
        , mBudget(budget)
    {
    }

    MemoryAllocator::~MemoryAllocator()
    {
        // Every resource this renderer makes is destroyed before the device it was made on, which is
        // what lets a block be freed here rather than counted. A block still standing a range means
        // something outlived the device, and freeing its memory would be the second thing wrong.
        assert(std::all_of(mBlocks.begin(), mBlocks.end(), [](const Block& block) { return block.mRuns.getEnd() == 0; })
            && "a device allocation was still standing a resource when the device went");
    }

    std::uint32_t MemoryAllocator::findType(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const
    {
        for (std::uint32_t i = 0; i < mMemory.memoryTypeCount; ++i)
        {
            const bool allowed = (typeBits & (1u << i)) != 0;
            const bool suitable = (mMemory.memoryTypes[i].propertyFlags & properties) == properties;
            if (allowed && suitable)
                return i;
        }

        throw Unsupported("no memory type has properties " + std::to_string(properties) + " among the "
            + std::to_string(mMemory.memoryTypeCount) + " this device offers");
    }

    VkDeviceSize MemoryAllocator::blockBytes(std::uint32_t type, std::uint32_t held) const
    {
        // A sixteenth, so that a heap far smaller than this hardware's — the host-visible window of
        // a card without resizable BAR is 256 MiB — is not carved into a handful of blocks.
        const VkDeviceSize heap = mMemory.memoryHeaps[mMemory.memoryTypes[type].heapIndex].size;
        const VkDeviceSize ceiling = std::min(sLargestBlock, heap / 16);

        // Doubled once per block the pool already stands. Written as a loop that stops at the
        // ceiling rather than as a shift, because a shift by the count would need a clamp of its
        // own and the count is what a long-lived pool grows without bound.
        VkDeviceSize wanted = sSmallestBlock;
        for (std::uint32_t doubled = 0; doubled < held && wanted < ceiling; ++doubled)
            wanted *= 2;

        return std::min(ceiling, wanted);
    }

    DeviceMemory MemoryAllocator::place(std::uint32_t at, Run run, VkDeviceSize alignment)
    {
        const Block& block = mBlocks[at];
        const VkDeviceSize offset = alignUp(VkDeviceSize{ run.mOffset } * sPage, alignment);
        void* const mapped = block.mMapped == nullptr ? nullptr : static_cast<std::byte*>(block.mMapped) + offset;

        return DeviceMemory(*this, at, run, block.mHandle.get(), offset, mapped);
    }

    DeviceMemory MemoryAllocator::take(
        const VkMemoryRequirements& requirements, VkMemoryPropertyFlags properties, Tiling tiling)
    {
        assert(requirements.size > 0);

        const std::lock_guard<std::mutex> held(mLock);

        const std::uint32_t type = findType(requirements.memoryTypeBits, properties);
        const std::uint32_t pool = poolOf(type, tiling);
        const std::uint32_t pages = pagesFor(requirements.size, requirements.alignment);

        if (pool >= mBlocksInPool.size())
            mBlocksInPool.resize(pool + 1, 0);

        // A slot whose allocation went back to the device stands nothing and can hold nothing. It
        // is remembered on the way past, because a block made below goes into one rather than
        // lengthening the list: a range names its block by index, and every index handed out has to
        // go on meaning what it meant.
        std::size_t retired = mBlocks.size();

        for (std::size_t at = 0; at < mBlocks.size(); ++at)
        {
            Block& block = mBlocks[at];
            if (block.mPages == 0)
            {
                retired = std::min(retired, at);
                continue;
            }

            if (block.mPool != pool)
                continue;

            // **Asked for and given back rather than measured first**, which is what
            // `StructureStorage` says of the same allocator: where a run goes is best fit over a
            // free list, and asking whether one would fit is that rule written a second time.
            const Run run = block.mRuns.allocate(pages);
            if (block.mRuns.getEnd() <= block.mPages)
                return place(static_cast<std::uint32_t>(at), run, requirements.alignment);

            block.mRuns.release(run);
        }

        const std::uint32_t made
            = std::max(static_cast<std::uint32_t>(blockBytes(type, mBlocksInPool[pool]) / sPage), pages);

        // **Built whole before it joins the list**, so that a device out of memory leaves the
        // allocator holding what it held rather than a block with no allocation behind it.
        Block block;
        block.mPool = pool;

        // **Every block, because a pool cannot know what will be put in it.** The flag costs a
        // device nothing it does not already pay for `bufferDeviceAddress`, which this renderer
        // requires; a pool that carried it only where the first resource asked would refuse the
        // second one that did.
        const VkMemoryAllocateFlagsInfo flags{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };

        const auto ask = [&](const std::uint32_t wanted) {
            const VkMemoryAllocateInfo allocate{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .pNext = &flags,
                .allocationSize = VkDeviceSize{ wanted } * sPage,
                .memoryTypeIndex = type,
            };

            const VkResult result = vkAllocateMemory(mDevice, &allocate, nullptr, block.mHandle.put(mDevice));
            if (result == VK_SUCCESS)
                block.mPages = wanted;

            return result;
        };

        // **The room the block wants is a preference; the room the resource needs is not.** A block
        // is sized from the heap, which is what the device has rather than what is left of it, so
        // another process holding most of the card turns the first request into a refusal where the
        // pages this one resource asked for would still have fitted.
        VkResult allocated = ask(made);
        if (allocated != VK_SUCCESS && made != pages)
            allocated = ask(pages);

        checkVk(allocated, "vkAllocateMemory");

        // **Mapped here rather than by whoever holds a range of it**, so the pointer goes when the
        // block does, and once for the whole block rather than once per resource in it.
        if ((mMemory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
            checkVk(vkMapMemory(mDevice, block.mHandle.get(), 0, VK_WHOLE_SIZE, 0, &block.mMapped), "vkMapMemory");

        ++mBlocksInPool[pool];

        const bool append = retired == mBlocks.size();
        if (append)
            mBlocks.push_back(std::move(block));
        else
            mBlocks[retired] = std::move(block);

        const auto at = static_cast<std::uint32_t>(append ? mBlocks.size() - 1 : retired);

        return place(at, mBlocks[at].mRuns.allocate(pages), requirements.alignment);
    }

    void MemoryAllocator::give(std::uint32_t block, Run run)
    {
        const std::lock_guard<std::mutex> locked(mLock);

        Block& held = mBlocks[block];
        held.mRuns.release(run);

        // The last block of a pool stays whatever happens: a pool that emptied and refilled would
        // otherwise free and allocate on alternate frames.
        if (held.mRuns.getEnd() > 0 || mBlocksInPool[held.mPool] <= 1)
            return;

        held.mHandle.reset();
        held.mMapped = nullptr;
        held.mPages = 0;
        --mBlocksInPool[held.mPool];
    }

    MemoryReport MemoryAllocator::report() const
    {
        const std::lock_guard<std::mutex> held(mLock);

        MemoryReport out;
        out.mHeapCount = std::min<std::uint32_t>(mMemory.memoryHeapCount, MemoryReport::sMaxHeaps);

        for (std::uint32_t heap = 0; heap < out.mHeapCount; ++heap)
            out.mHeaps[heap].mSize = mMemory.memoryHeaps[heap].size;

        // **What a heap is for, taken off its types rather than off the heap.** Vulkan states the
        // host's access on the memory type and only the device's on the heap, so a heap is
        // host-visible here when any type in it is — which is what makes the small aperture of a
        // card without resizable BAR tell itself apart from the video memory beside it.
        for (std::uint32_t type = 0; type < mMemory.memoryTypeCount; ++type)
        {
            const std::uint32_t heap = mMemory.memoryTypes[type].heapIndex;
            if (heap < out.mHeapCount && (mMemory.memoryTypes[type].propertyFlags & sHostWritten) == sHostWritten)
                out.mHeaps[heap].mHostVisible = true;
        }

        for (const Block& block : mBlocks)
        {
            if (block.mPages == 0)
                continue;

            const std::uint32_t type = typeOf(block.mPool);
            const VkDeviceSize reserved = VkDeviceSize{ block.mPages } * sPage;
            const VkDeviceSize live = VkDeviceSize{ block.mRuns.getEnd() - block.mRuns.getFree() } * sPage;

            if ((mMemory.memoryTypes[type].propertyFlags & sHostWritten) == sHostWritten)
            {
                out.mHostWrittenReserved += reserved;
                out.mHostWrittenLive += live;
            }

            const std::uint32_t heap = mMemory.memoryTypes[type].heapIndex;
            if (heap >= out.mHeapCount)
                continue;

            HeapUse& use = out.mHeaps[heap];
            use.mReserved += reserved;
            use.mLive += live;
            ++use.mBlocks;
        }

        if (mBudget)
        {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
            };
            VkPhysicalDeviceMemoryProperties2 properties{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                .pNext = &budget,
            };
            vkGetPhysicalDeviceMemoryProperties2(mPhysicalDevice, &properties);

            for (std::uint32_t heap = 0; heap < out.mHeapCount; ++heap)
            {
                out.mHeaps[heap].mBudget = budget.heapBudget[heap];
                out.mHeaps[heap].mHeld = budget.heapUsage[heap];
            }
        }

        return out;
    }

    std::size_t MemoryAllocator::getBlockCount() const
    {
        const std::lock_guard<std::mutex> held(mLock);

        return static_cast<std::size_t>(
            std::count_if(mBlocks.begin(), mBlocks.end(), [](const Block& block) { return block.mPages > 0; }));
    }
}

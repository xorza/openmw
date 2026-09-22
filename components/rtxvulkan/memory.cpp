#include "memory.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <components/rtx/error.hpp>

#include "requirements.hpp"
#include "result.hpp"

// The one translation unit that holds the library's body. Told the two entry points every other
// function is looked up through, because the loader this links exports Vulkan 1.4 and the
// header's static path would name every one of those by hand.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

namespace Rtx
{
    namespace
    {
        /// The block a memory type is grown by. Sixty-four megabytes: what this fork's own allocator
        /// settled on before the library took over, against the library's quarter of a gigabyte,
        /// which on a card whose host-visible video memory is a couple of hundred megabytes is the
        /// heap in one block.
        constexpr VkDeviceSize sBlockBytes = 64 * 1024 * 1024;

        VmaAllocationCreateInfo askingFor(const VkMemoryPropertyFlags properties)
        {
            VmaAllocationCreateInfo create{};
            create.requiredFlags = properties;
            if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
                create.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            return create;
        }

        /// What the library answered, as this renderer's errors: a memory type nobody offers is a
        /// wrong request on hardware that qualifies, and everything else is the device's.
        void checkAllocated(const VkResult result, const VkMemoryPropertyFlags properties)
        {
            if (result == VK_ERROR_FEATURE_NOT_PRESENT)
                throw Unsupported(
                    "no memory type has properties " + std::to_string(properties) + " among those this device offers");

            checkVk(result, "vmaAllocateMemory");
        }

        /// Why `tryTake` refused: the one reason, because what stands in does not depend on
        /// which use ran out first.
        constexpr std::string_view sNoRoom = "no device memory is left for it";
    }

    DeviceMemory::DeviceMemory(MemoryAllocator* const owner, VmaAllocation_T* const allocation,
        const VkDeviceMemory handle, const VkDeviceSize offset, void* const mapped, const VkDeviceSize size,
        const std::uint32_t heap, const MemoryUse use)
        : mOwner(owner)
        , mAllocation(allocation)
        , mHandle(handle)
        , mOffset(offset)
        , mMapped(mapped)
        , mSize(size)
        , mHeap(heap)
        , mUse(use)
    {
    }

    DeviceMemory::~DeviceMemory()
    {
        release();
    }

    DeviceMemory::DeviceMemory(DeviceMemory&& other) noexcept
        : mOwner(std::exchange(other.mOwner, nullptr))
        , mAllocation(std::exchange(other.mAllocation, nullptr))
        , mHandle(std::exchange(other.mHandle, VK_NULL_HANDLE))
        , mOffset(std::exchange(other.mOffset, 0))
        , mMapped(std::exchange(other.mMapped, nullptr))
        , mSize(std::exchange(other.mSize, 0))
        , mHeap(other.mHeap)
        , mUse(other.mUse)
    {
    }

    DeviceMemory& DeviceMemory::operator=(DeviceMemory&& other) noexcept
    {
        if (this != &other)
        {
            release();

            mOwner = std::exchange(other.mOwner, nullptr);
            mAllocation = std::exchange(other.mAllocation, nullptr);
            mHandle = std::exchange(other.mHandle, VK_NULL_HANDLE);
            mOffset = std::exchange(other.mOffset, 0);
            mMapped = std::exchange(other.mMapped, nullptr);
            mSize = std::exchange(other.mSize, 0);
            mHeap = other.mHeap;
            mUse = other.mUse;
        }

        return *this;
    }

    void DeviceMemory::release()
    {
        if (mOwner != nullptr)
            std::exchange(mOwner, nullptr)->give(*this);
    }

    MemoryAllocator::MemoryAllocator(const VkInstance instance, const VkPhysicalDevice physicalDevice,
        const VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, const bool budget)
        : mDevice(device)
        , mMemory(memory)
        , mBudget(budget)
    {
        VmaVulkanFunctions functions{};
        functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

        // Every allocation may back a buffer that is addressed, so every one carries the flag.
        VmaAllocatorCreateInfo create{};
        create.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        if (budget)
            create.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        create.physicalDevice = physicalDevice;
        create.device = device;
        create.instance = instance;
        create.vulkanApiVersion = sApiVersion;
        create.preferredLargeHeapBlockSize = sBlockBytes;
        create.pVulkanFunctions = &functions;

        checkVk(vmaCreateAllocator(&create, &mAllocator), "vmaCreateAllocator");

        // The type the library picks for a resource that asks for video memory and nothing else,
        // which is what every structure and texture asks for.
        const VmaAllocationCreateInfo video = askingFor(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checkVk(vmaFindMemoryTypeIndex(mAllocator, ~0u, &video, &mVideoType), "vmaFindMemoryTypeIndex");
        mVideoHeap = mMemory.memoryTypes[mVideoType].heapIndex;

        // Made now and not when first asked for, so no thread ever makes one: a pool with no block
        // holds nothing.
        for (std::uint32_t type = 0; type < mMemory.memoryTypeCount; ++type)
        {
            if ((mMemory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0)
                continue;

            const VmaPoolCreateInfo pool{ .memoryTypeIndex = type, .blockSize = sBlockBytes };
            checkVk(vmaCreatePool(mAllocator, &pool, &mContentPools[type]), "vmaCreatePool");
        }
    }

    MemoryAllocator::~MemoryAllocator()
    {
        // The ranges and not the blocks: the library keeps an emptied block or two against the
        // next resource, which a range still standing in one is not.
        assert(getLiveCount() == 0 && "a device allocation was still standing a resource when the device went");

        for (VmaPool_T* const pool : mContentPools)
            if (pool != nullptr)
                vmaDestroyPool(mAllocator, pool);

        vmaDestroyAllocator(mAllocator);
    }

    DeviceMemory MemoryAllocator::take(
        const VkBuffer buffer, const VkMemoryPropertyFlags properties, const VkDeviceSize alignment)
    {
        assert(alignment > 0 && (alignment & (alignment - 1)) == 0 && "an alignment is a power of two");

        // The requirements by hand and not the library's own look at the buffer, because that look
        // takes the driver's alignment and no other, and a scratch buffer owes a coarser one. What
        // the library then does not know is that this is a buffer, so it keeps the image
        // granularity between this and any neighbour — a kilobyte on this hardware, which is what
        // every range paid before it.
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(mDevice, buffer, &requirements);
        requirements.alignment = std::max(requirements.alignment, alignment);

        const VmaAllocationCreateInfo create = askingFor(properties);
        VmaAllocation allocation = nullptr;
        VmaAllocationInfo placed{};
        checkAllocated(vmaAllocateMemory(mAllocator, &requirements, &create, &allocation, &placed), properties);

        return hold(allocation, placed.deviceMemory, placed.offset, placed.pMappedData, placed.size, placed.memoryType,
            MemoryUse::Essential);
    }

    DeviceMemory MemoryAllocator::take(const VkImage image, const VkMemoryPropertyFlags properties)
    {
        const VmaAllocationCreateInfo create = askingFor(properties);
        VmaAllocation allocation = nullptr;
        VmaAllocationInfo placed{};
        checkAllocated(vmaAllocateMemoryForImage(mAllocator, image, &create, &allocation, &placed), properties);

        return hold(allocation, placed.deviceMemory, placed.offset, placed.pMappedData, placed.size, placed.memoryType,
            MemoryUse::Essential);
    }

    Result<DeviceMemory, std::string_view> MemoryAllocator::tryTake(const VkBuffer buffer,
        const VkMemoryPropertyFlags properties, const VkDeviceSize alignment, const MemoryUse use)
    {
        if (use == MemoryUse::Essential)
            return take(buffer, properties, alignment);

        assert(alignment > 0 && (alignment & (alignment - 1)) == 0 && "an alignment is a power of two");

        // By hand, for the reason `take` gives.
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(mDevice, buffer, &requirements);
        requirements.alignment = std::max(requirements.alignment, alignment);

        return tryAllocate(requirements.size, requirements.memoryTypeBits, false, properties, use,
            [&](const VmaAllocationCreateInfo& create, VmaAllocation* allocation, VmaAllocationInfo* placed) {
                return vmaAllocateMemory(mAllocator, &requirements, &create, allocation, placed);
            });
    }

    Result<DeviceMemory, std::string_view> MemoryAllocator::tryTake(
        const VkImage image, const VkMemoryPropertyFlags properties, const MemoryUse use)
    {
        if (use == MemoryUse::Essential)
            return take(image, properties);

        // Whether the driver binds the image to nothing but an allocation of its own, which the
        // library would otherwise find out only inside a pool that makes none.
        VkMemoryDedicatedRequirements dedicated{ .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
        VkMemoryRequirements2 requirements{ .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated };
        const VkImageMemoryRequirementsInfo2 info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            .image = image,
        };
        vkGetImageMemoryRequirements2(mDevice, &info, &requirements);

        return tryAllocate(requirements.memoryRequirements.size, requirements.memoryRequirements.memoryTypeBits,
            dedicated.requiresDedicatedAllocation == VK_TRUE, properties, use,
            [&](const VmaAllocationCreateInfo& create, VmaAllocation* allocation, VmaAllocationInfo* placed) {
                return vmaAllocateMemoryForImage(mAllocator, image, &create, allocation, placed);
            });
    }

    template <class Allocate>
    Result<DeviceMemory, std::string_view> MemoryAllocator::tryAllocate(const VkDeviceSize size,
        const std::uint32_t typeBits, bool own, const VkMemoryPropertyFlags properties, const MemoryUse use,
        Allocate&& allocate)
    {
        VmaAllocationCreateInfo create = askingFor(properties);
        std::uint32_t type = 0;
        checkAllocated(vmaFindMemoryTypeIndex(mAllocator, typeBits, &create, &type), properties);
        assert(mContentPools[type] != nullptr && "content asked for memory that is not video memory");

        // Larger than half a block, an allocation of its own, as the library gives one outside a
        // pool: a block past half full with one resource is a block the next one does not fit.
        own |= size > sBlockBytes / 2;

        VmaAllocation allocation = nullptr;
        VmaAllocationInfo placed{};
        if (own)
            create.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        else
        {
            // Room in a block content already holds costs the heap nothing, so it is weighed
            // against nothing: a use at its ceiling still fills the gaps its own departures left.
            create.pool = mContentPools[type];
            create.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
            const VkResult inBlock = allocate(create, &allocation, &placed);
            if (inBlock == VK_SUCCESS)
                return hold(allocation, placed.deviceMemory, placed.offset, placed.pMappedData, placed.size,
                    placed.memoryType, use);

            if (inBlock != VK_ERROR_OUT_OF_DEVICE_MEMORY)
                checkAllocated(inBlock, properties);

            create.flags &= ~VmaAllocationCreateFlags{ VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT };
        }

        // New memory, which is exactly a block or exactly the resource.
        const std::uint32_t heap = mMemory.memoryTypes[type].heapIndex;
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(mAllocator, budgets);
        const VmaBudget& budget = budgets[heap];
        if (budget.usage + (own ? size : sBlockBytes)
            > ceilingOf(heap, use, budget.budget, budget.usage, budget.statistics.blockBytes))
            return Err{ sNoRoom };

        // A driver refusing what its budget said it had is the same answer, and the one `tryTake`
        // exists to give.
        const VkResult result = allocate(create, &allocation, &placed);
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
            return Err{ sNoRoom };

        checkAllocated(result, properties);
        return hold(
            allocation, placed.deviceMemory, placed.offset, placed.pMappedData, placed.size, placed.memoryType, use);
    }

    DeviceMemory MemoryAllocator::hold(VmaAllocation_T* const allocation, const VkDeviceMemory handle,
        const VkDeviceSize offset, void* const mapped, const VkDeviceSize size, const std::uint32_t type,
        const MemoryUse use)
    {
        const std::uint32_t heap = mMemory.memoryTypes[type].heapIndex;
        mHeld[heap][static_cast<std::size_t>(use)] += size;

        return DeviceMemory(this, allocation, handle, offset, mapped, size, heap, use);
    }

    void MemoryAllocator::give(DeviceMemory& memory)
    {
        mHeld[memory.mHeap][static_cast<std::size_t>(memory.mUse)] -= memory.mSize;
        vmaFreeMemory(mAllocator, memory.mAllocation);
    }

    VkDeviceSize MemoryAllocator::ceilingOf(const std::uint32_t heap, const MemoryUse use, const VkDeviceSize budget,
        const VkDeviceSize usage, const VkDeviceSize blockBytes) const
    {
        VkDeviceSize owed = usage > blockBytes ? usage - blockBytes : 0;
        for (std::size_t before = 0; before < static_cast<std::size_t>(use); ++before)
            owed += mHeld[heap][before];

        const VkDeviceSize heldTo = std::min(budget, mBudgetLimit.value_or(budget));
        return heldTo > owed ? heldTo - owed : 0;
    }

    VkDeviceSize MemoryAllocator::getRoom(const MemoryUse use) const
    {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(mAllocator, budgets);
        const VmaBudget& budget = budgets[mVideoHeap];

        VmaStatistics content{};
        vmaGetPoolStatistics(mAllocator, mContentPools[mVideoType], &content);

        const VkDeviceSize ceiling
            = ceilingOf(mVideoHeap, use, budget.budget, budget.usage, budget.statistics.blockBytes);
        const VkDeviceSize above = ceiling > budget.usage ? ceiling - budget.usage : 0;

        // Whole blocks, because that is what the small resources `tryAllocate` places are placed in.
        return content.blockBytes - content.allocationBytes + above / sBlockBytes * sBlockBytes;
    }

    VkDeviceSize MemoryAllocator::getHeld(const std::uint32_t heap, const MemoryUse use) const
    {
        assert(heap < VK_MAX_MEMORY_HEAPS);
        return mHeld[heap][static_cast<std::size_t>(use)];
    }

    void MemoryAllocator::limitBudget(const std::optional<VkDeviceSize> bytes)
    {
        mBudgetLimit = bytes;
    }

    void MemoryAllocator::refreshBudget(const std::uint64_t frame)
    {
        vmaSetCurrentFrameIndex(mAllocator, static_cast<std::uint32_t>(frame));
    }

    std::size_t MemoryAllocator::getLiveCount() const
    {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(mAllocator, budgets);

        std::size_t ranges = 0;
        for (std::uint32_t heap = 0; heap < mMemory.memoryHeapCount; ++heap)
            ranges += budgets[heap].statistics.allocationCount;

        return ranges;
    }

    std::size_t MemoryAllocator::getBlockCount() const
    {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(mAllocator, budgets);

        std::size_t blocks = 0;
        for (std::uint32_t heap = 0; heap < mMemory.memoryHeapCount; ++heap)
            blocks += budgets[heap].statistics.blockCount;

        return blocks;
    }

    MemoryReport MemoryAllocator::report() const
    {
        MemoryReport out;
        out.mHeapCount = std::min<std::uint32_t>(mMemory.memoryHeapCount, MemoryReport::sMaxHeaps);

        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(mAllocator, budgets);

        for (std::uint32_t heap = 0; heap < out.mHeapCount; ++heap)
        {
            HeapUse& use = out.mHeaps[heap];
            use.mSize = mMemory.memoryHeaps[heap].size;
            use.mDeviceLocal = (mMemory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
            use.mReserved = budgets[heap].statistics.blockBytes;
            use.mLive = budgets[heap].statistics.allocationBytes;
            use.mBlocks = budgets[heap].statistics.blockCount;

            // Left at nought without the extension, where the library would estimate: a reader
            // tells a driver that would not say from one that said none by the columns' absence.
            if (mBudget)
            {
                use.mBudget = std::min(budgets[heap].budget, mBudgetLimit.value_or(budgets[heap].budget));
                use.mHeld = budgets[heap].usage;
            }
        }

        // The host-written figures are per memory type, which the budgets do not split, so this is
        // the walk over every allocation the header says a report is.
        VmaTotalStatistics statistics{};
        vmaCalculateStatistics(mAllocator, &statistics);

        for (std::uint32_t type = 0; type < mMemory.memoryTypeCount; ++type)
        {
            if ((mMemory.memoryTypes[type].propertyFlags & sHostWritten) != sHostWritten)
                continue;

            const std::uint32_t heap = mMemory.memoryTypes[type].heapIndex;
            if (heap < out.mHeapCount)
                out.mHeaps[heap].mHostVisible = true;

            out.mHostWrittenReserved += statistics.memoryType[type].statistics.blockBytes;
            out.mHostWrittenLive += statistics.memoryType[type].statistics.allocationBytes;
        }

        return out;
    }
}

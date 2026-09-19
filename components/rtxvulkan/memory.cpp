#include "memory.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
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
    }

    DeviceMemory::DeviceMemory(VmaAllocator_T* const owner, VmaAllocation_T* const allocation,
        const VkDeviceMemory handle, const VkDeviceSize offset, void* const mapped)
        : mOwner(owner)
        , mAllocation(allocation)
        , mHandle(handle)
        , mOffset(offset)
        , mMapped(mapped)
    {
    }

    DeviceMemory::~DeviceMemory()
    {
        if (mOwner != nullptr)
            vmaFreeMemory(mOwner, mAllocation);
    }

    DeviceMemory::DeviceMemory(DeviceMemory&& other) noexcept
        : mOwner(std::exchange(other.mOwner, nullptr))
        , mAllocation(std::exchange(other.mAllocation, nullptr))
        , mHandle(std::exchange(other.mHandle, VK_NULL_HANDLE))
        , mOffset(std::exchange(other.mOffset, 0))
        , mMapped(std::exchange(other.mMapped, nullptr))
    {
    }

    DeviceMemory& DeviceMemory::operator=(DeviceMemory&& other) noexcept
    {
        if (this != &other)
        {
            if (mOwner != nullptr)
                vmaFreeMemory(mOwner, mAllocation);

            mOwner = std::exchange(other.mOwner, nullptr);
            mAllocation = std::exchange(other.mAllocation, nullptr);
            mHandle = std::exchange(other.mHandle, VK_NULL_HANDLE);
            mOffset = std::exchange(other.mOffset, 0);
            mMapped = std::exchange(other.mMapped, nullptr);
        }

        return *this;
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
    }

    MemoryAllocator::~MemoryAllocator()
    {
        // The ranges and not the blocks: the library keeps an emptied block or two against the
        // next resource, which a range still standing in one is not.
        assert(getLiveCount() == 0 && "a device allocation was still standing a resource when the device went");
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

        return DeviceMemory(mAllocator, allocation, placed.deviceMemory, placed.offset, placed.pMappedData);
    }

    DeviceMemory MemoryAllocator::take(const VkImage image, const VkMemoryPropertyFlags properties)
    {
        const VmaAllocationCreateInfo create = askingFor(properties);
        VmaAllocation allocation = nullptr;
        VmaAllocationInfo placed{};
        checkAllocated(vmaAllocateMemoryForImage(mAllocator, image, &create, &allocation, &placed), properties);

        return DeviceMemory(mAllocator, allocation, placed.deviceMemory, placed.offset, placed.pMappedData);
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
                use.mBudget = budgets[heap].budget;
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

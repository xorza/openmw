#include "memorylimits.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <vulkan/vulkan_core.h>

#include <components/rtx/memoryreport.hpp>
#include <components/rtx/result.hpp>

namespace Rtx::Testing
{
    BudgetLimit::BudgetLimit(MemoryAllocator& memory, const VkDeviceSize bytes)
        : mMemory(memory)
    {
        mMemory.limitBudget(bytes);
    }

    BudgetLimit::~BudgetLimit()
    {
        mMemory.limitBudget(std::nullopt);
    }

    VkDeviceSize budgetAbove(const MemoryAllocator& memory, const MemoryUse use, const VkDeviceSize above)
    {
        const std::uint32_t heap = memory.getVideoHeap();
        const HeapUse held = memory.report().mHeaps[heap];
        if (held.mHeld == 0)
            throw std::runtime_error("a device with no budget extension cannot say what the heap holds");

        VkDeviceSize owed = held.mHeld - held.mReserved;
        for (std::size_t before = 0; before < static_cast<std::size_t>(use); ++before)
            owed += memory.getHeld(heap, static_cast<MemoryUse>(before));

        return held.mHeld + owed + above;
    }

    NoRoomForContent::NoRoomForContent(const Device& device)
        : mNone(device.getMemory(), 0)
    {
        // Largest first, so the gaps fill in a few dozen buffers and not thousands, down to the
        // structure alignment, below which no resource content asks for is placed.
        for (VkDeviceSize size = VkDeviceSize{ 16 } << 20; size >= 256; size /= 16)
            while (true)
            {
                Result<Buffer, std::string_view> filler = Buffer::tryMake(MemoryUse::Texture, device,
                    BufferKind::DeviceLocal, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "content filler");
                if (!filler.isOk())
                    break;

                mFillers.push_back(std::move(filler.value()));
            }
    }
}

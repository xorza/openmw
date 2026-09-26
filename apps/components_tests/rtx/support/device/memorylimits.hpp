#pragma once

#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/memory.hpp>

namespace Rtx::Testing
{
    /// What `MemoryAllocator::limitBudget` is set to for as long as one stands, and taken off
    /// after: the device is the binary's, and a limit left behind is every later test's.
    class BudgetLimit
    {
    public:
        BudgetLimit(MemoryAllocator& memory, VkDeviceSize bytes);
        ~BudgetLimit();

        BudgetLimit(const BudgetLimit&) = delete;
        BudgetLimit& operator=(const BudgetLimit&) = delete;

    private:
        MemoryAllocator& mMemory;
    };

    /// The budget at which `use`'s ceiling stands `above` over what the video heap holds now:
    /// what the heap holds, and once more what the process holds outside the allocator and what
    /// every use before `use` holds. Throws on a device with no budget extension, which cannot
    /// say what the heap holds.
    VkDeviceSize budgetAbove(const MemoryAllocator& memory, MemoryUse use, VkDeviceSize above);

    /// Video memory with no room left for content, for as long as one stands: every ceiling at
    /// nought, and every gap in content's own blocks filled, so the next structure or texture is
    /// refused however small it is — a card that has run out, as an arrival meets one. The
    /// frame's own memory is still made, in blocks content never shares. Lifted on the way out,
    /// the fillers before the limit.
    class NoRoomForContent
    {
    public:
        explicit NoRoomForContent(const Device& device);

    private:
        BudgetLimit mNone;
        std::vector<Buffer> mFillers;
    };
}

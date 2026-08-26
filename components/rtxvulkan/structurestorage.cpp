#include "structurestorage.hpp"

#include <string>
#include <utility>

#include <components/rtx/error.hpp>

#include "device.hpp"

namespace Rtx
{
    namespace
    {
        /// What an acceleration structure's offset in its buffer has to be a multiple of, and so the
        /// unit the allocator hands out. Vulkan fixes it at 256.
        constexpr VkDeviceSize sAlignment = 256;

        std::uint32_t unitsFor(VkDeviceSize bytes)
        {
            return static_cast<std::uint32_t>((bytes + sAlignment - 1) / sAlignment);
        }
    }

    StructureStorage::StructureStorage(VkBufferUsageFlags usage, std::string name)
        : mUsage(usage)
        , mName(std::move(name))
    {
    }

    StructureRoom StructureStorage::take(const Device& device, VkDeviceSize bytes, VkDeviceSize least)
    {
        assert(bytes > 0);
        const std::uint32_t units = unitsFor(bytes);

        for (std::size_t at = 0; at < mBlocks.size(); ++at)
        {
            Block& block = mBlocks[at];

            // **Asked for and given back rather than measured first.** The allocator's rule for
            // where a run goes is best fit over a free list, and reimplementing it here to ask
            // whether it would fit is two answers to one question; a run given back at the end
            // shrinks the reach it just extended.
            const Span run = block.mRuns.allocate(units);
            if (block.mRuns.getEnd() <= block.mUnits)
                return StructureRoom{ static_cast<std::uint32_t>(at), run };

            block.mRuns.release(run);
        }

        const std::uint32_t made = std::max(unitsFor(least), units);

        Block& block = mBlocks.emplace_back();
        block.mUnits = made;
        block.mBuffer = Buffer(device, VkDeviceSize{ made } * sAlignment, mUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        device.setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(block.mBuffer.getHandle()),
            mName + " " + std::to_string(mBlocks.size() - 1));

        return StructureRoom{ static_cast<std::uint32_t>(mBlocks.size() - 1), block.mRuns.allocate(units) };
    }

    void StructureStorage::give(const StructureRoom& room)
    {
        if (room.empty())
            return;

        mBlocks[room.mBlock].mRuns.release(room.mRun);
    }

    VkDeviceSize StructureStorage::getOffset(const StructureRoom& room) const
    {
        return VkDeviceSize{ room.mRun.mOffset } * sAlignment;
    }

    VkDeviceSize StructureStorage::getBytes() const
    {
        VkDeviceSize total = 0;
        for (const Block& block : mBlocks)
            total += block.mBuffer.getSize();

        return total;
    }
}

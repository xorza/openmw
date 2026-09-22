#include "structurestorage.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "device.hpp"
#include "memory.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t unitsFor(VkDeviceSize bytes)
        {
            return static_cast<std::uint32_t>(
                alignUp(bytes, StructureStorage::sAlignment) / StructureStorage::sAlignment);
        }
    }

    StructureStorage::StructureStorage(VkBufferUsageFlags usage, std::string name)
        : mUsage(usage)
        , mName(std::move(name))
    {
    }

    Result<StructureRoom, std::string_view> StructureStorage::take(
        const Device& device, VkDeviceSize bytes, VkDeviceSize least)
    {
        assert(bytes > 0);

        // Every live block may hold a structure, so the list is asked for the first that fits,
        // and a new block is as large as the caller asked or as the structure needs — as the
        // structure needs and no more, where the device has no room for what the caller asked.
        return mBlocks.take(
            unitsFor(bytes), [](const Block&) { return true; },
            [&](const std::uint32_t units, const std::uint32_t slot) -> Result<Block, std::string_view> {
                // Named only where a capture could read it: a release build names nothing, and
                // the concatenation is a trip to the heap for a name that goes nowhere.
                std::string name;
                if constexpr (Device::wantsNames())
                    name = mName + " " + std::to_string(slot);

                const auto make = [&](const std::uint32_t capacity) {
                    return Buffer::tryMake(MemoryUse::Structure, device, BufferKind::DeviceLocal,
                        VkDeviceSize{ capacity } * sAlignment, mUsage, name);
                };

                std::uint32_t made = std::max(unitsFor(least), units);
                Result<Buffer, std::string_view> buffer = make(made);
                if (!buffer.isOk() && made > units)
                {
                    made = units;
                    buffer = make(made);
                }

                if (!buffer.isOk())
                    return Err{ buffer.error() };

                Block block;
                block.mCapacity = made;
                block.mBuffer = std::move(buffer.value());
                return block;
            });
    }

    void StructureStorage::give(const StructureRoom& room)
    {
        if (room.empty())
            return;

        // A block that empties goes back to the device, one at a time and never a sweep. A
        // refitted structure stays for the life of its mesh and pins its block. The last one
        // standing stays, so a scene that empties and fills does not ask for it back on the next
        // arrival.
        mBlocks.give(room, [&](const Block&) { return countLive() > 1; });
    }

    std::size_t StructureStorage::countLive() const
    {
        std::size_t live = 0;
        for (const Block& block : mBlocks)
            if (block.mCapacity > 0)
                ++live;

        return live;
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

    VkDeviceSize StructureStorage::getLiveBytes() const
    {
        VkDeviceSize total = 0;
        for (const Block& block : mBlocks)
            total += VkDeviceSize{ block.mRuns.getUsed() } * sAlignment;

        return total;
    }
}

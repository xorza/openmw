#include "structurestorage.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <utility>

#include <components/rtx/error.hpp>

#include "device.hpp"

namespace Rtx
{
    namespace
    {
        std::uint32_t unitsFor(VkDeviceSize bytes)
        {
            return static_cast<std::uint32_t>(
                (bytes + StructureStorage::sAlignment - 1) / StructureStorage::sAlignment);
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

        // **A place a retired block left, remembered on the way past.** A room names its block by
        // index, so nothing is ever erased from this list; without filling the empty places again, a
        // route that compacts at every crossing would grow one place per block it ever made.
        std::size_t spare = mBlocks.size();

        for (std::size_t at = 0; at < mBlocks.size(); ++at)
        {
            Block& block = mBlocks[at];
            if (block.mUnits == 0)
            {
                spare = std::min(spare, at);
                continue;
            }

            // **Asked for and given back rather than measured first.** The allocator's rule for
            // where a run goes is best fit over a free list, and reimplementing it here to ask
            // whether it would fit is two answers to one question; a run given back at the end
            // shrinks the reach it just extended.
            const Run run = block.mRuns.allocate(units);
            if (block.mRuns.getEnd() <= block.mUnits)
                return StructureRoom{ static_cast<std::uint32_t>(at), run };

            block.mRuns.release(run);
        }

        const std::uint32_t made = std::max(unitsFor(least), units);

        if (spare == mBlocks.size())
            mBlocks.emplace_back();

        Block& block = mBlocks[spare];
        block.mUnits = made;
        block.mBuffer = Buffer::deviceLocal(device, VkDeviceSize{ made } * sAlignment, mUsage);
        device.setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(block.mBuffer.getHandle()),
            mName + " " + std::to_string(spare));

        return StructureRoom{ static_cast<std::uint32_t>(spare), block.mRuns.allocate(units) };
    }

    void StructureStorage::give(const StructureRoom& room)
    {
        if (room.empty())
            return;

        Block& block = mBlocks[room.mBlock];
        block.mRuns.release(room.mRun);

        // **A block that empties goes back to the device, one at a time and never a sweep.**
        // Compaction is what leaves whole blocks empty: a structure copied tight gives the loose
        // room it stood in back, and a block holding nothing else has nothing left to hold.
        //
        // **A block empties only where everything in it could leave**, so what a caller mixes into
        // one block decides whether this ever fires: a structure that is refitted rather than
        // replaced stays for the life of its mesh and pins the block it sits in.
        //
        // **The last one standing stays**, so a scene that empties and fills does not give its only
        // block back and ask for another on the next arrival.
        if (block.mRuns.getEnd() == 0 && countLive() > 1)
        {
            block.mBuffer = Buffer();
            block.mUnits = 0;
        }
    }

    std::size_t StructureStorage::countLive() const
    {
        std::size_t live = 0;
        for (const Block& block : mBlocks)
            if (block.mUnits > 0)
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

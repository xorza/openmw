#include "blockedbuffer.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "barriers.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "memory.hpp"

namespace Rtx
{
    void BlockedBuffer::open(const Device& device, VkBufferUsageFlags usage, std::string_view name)
    {
        assert(mDevice == nullptr && "a blocked buffer opened twice");

        mDevice = &device;

        // A shader reaches a block through a pointer out of the table, and a buffer only has an
        // address if it was created saying so. `TRANSFER_DST` because a block is filled and written
        // by the device rather than by the host: `writeAt` says why.
        mUsage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        // Kept only where a capture could read them: a release build names nothing, and building
        // them is a trip to the heap apiece.
        if constexpr (Device::wantsNames())
        {
            mName = name;
            mTableName = std::string(name) + " blocks";
        }
    }

    void BlockedBuffer::reserve(Batch& batch, std::uint32_t elements)
    {
        assert(mDevice != nullptr && "a blocked buffer written before it was opened");

        const std::uint32_t wanted
            = std::max(1u, static_cast<std::uint32_t>(alignUp(elements, mBlockSize) / mBlockSize));
        if (wanted <= mBlocks.size())
            return;

        while (mBlocks.size() < wanted)
        {
            std::string name;
            if constexpr (Device::wantsNames())
                name = mName + " " + std::to_string(mBlocks.size());

            Buffer made = Buffer::deviceLocal(*mDevice, getBlockBytes(), mUsage, name);

            // Zeroed at birth, not left as the allocator found it. A block is longer than what
            // is put in it and holds gaps between the runs handed out, and a picture that depended
            // on what was last in that memory would depend on it.
            made.clear(batch.getCommands());

            mAddresses.push_back(made.getDeviceAddress());
            mBlocks.push_back(std::move(made));
        }

        // The fills above and the copies after them write the same bytes. A run written into a
        // block this call just emptied is a write after a write, and the queue orders neither
        // against the other on its own — the layers say so at once, which is how this was found.
        handOver(batch.getCommands(), Use::sBufferClearWrite,
            BufferUse{ VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT });

        // Made again rather than appended to, which is what a table of a few dozen addresses is
        // worth: the address changes, and every frame carries it afresh. Addressable and never
        // bound, because the frame block is how a shader reaches it.
        //
        // **The old one goes to the batch and not to the floor.** A frame in flight carries its
        // address in its frame block and reads it on every hit, so it lives until the batch's own
        // submit is fenced — which is what `Batch::keep` promises of the staging, and is after
        // that frame. Destroyed here, it was the invalid read at a fixed address that lost the
        // device on the first arrival with two frames in flight.
        batch.keep(std::move(mTable));
        mTable = Buffer::hostWritten(*mDevice, mAddresses.size() * sizeof(VkDeviceAddress),
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, mTableName);
        mTable.write(std::span<const VkDeviceAddress>(mAddresses));
    }

    void BlockedBuffer::writeInto(Batch& batch, std::uint32_t element, std::span<const std::byte> bytes)
    {
        assert(blockOf(element) < mBlocks.size());

        stageInto(batch, mBlocks[blockOf(element)], offsetOf(element), bytes);
    }
}

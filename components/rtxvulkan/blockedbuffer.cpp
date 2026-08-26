#include "blockedbuffer.hpp"

#include <cstring>

#include "device.hpp"

namespace Rtx
{
    void BlockedBuffer::open(const Device& device, VkBufferUsageFlags usage, std::string_view name)
    {
        assert(mDevice == nullptr && "a blocked buffer opened twice");

        mDevice = &device;

        // A shader reaches a block through a pointer out of the table, and a buffer only has an
        // address if it was created saying so.
        mUsage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        mName = name;
    }

    void BlockedBuffer::reserve(std::uint32_t elements)
    {
        assert(mDevice != nullptr && "a blocked buffer written before it was opened");

        const std::uint32_t wanted = std::max(1u, (elements + mBlockSize - 1) / mBlockSize);
        if (wanted <= mBlocks.size())
            return;

        while (mBlocks.size() < wanted)
        {
            HostBuffer made(*mDevice, getBlockBytes(), mUsage);

            // **Zeroed at birth, not left as the allocator found it.** A block is longer than what
            // is put in it and holds gaps between the runs handed out, and a picture that depended
            // on what was last in that memory would depend on it.
            made.clear();

            mDevice->setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(made.getHandle()),
                mName + " " + std::to_string(mBlocks.size()));
            mAddresses.push_back(made.getDeviceAddress());
            mBlocks.push_back(std::move(made));
        }

        // Made again rather than appended to, which is what a table of a few dozen addresses is
        // worth: the handle changes, and every descriptor naming it is pushed afresh each frame.
        mTable = HostBuffer(*mDevice, mAddresses.size() * sizeof(VkDeviceAddress), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        mTable.write(std::span<const VkDeviceAddress>(mAddresses));
        mDevice->setName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(mTable.getHandle()), mName + " blocks");
    }
}

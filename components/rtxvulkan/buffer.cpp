#include "buffer.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

#include "barriers.hpp"
#include "device.hpp"
#include "result.hpp"
#include "timeline.hpp"

namespace Rtx
{
    namespace
    {
        /// The memory each kind is made in.
        VkMemoryPropertyFlags propertiesOf(const BufferKind kind)
        {
            switch (kind)
            {
                case BufferKind::DeviceLocal:
                    return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                case BufferKind::HostWritten:
                    return sHostWritten;
                case BufferKind::Staging:
                    return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            }

            return 0;
        }
    }

    Buffer::Buffer(const Device& device, const BufferKind kind, const VkDeviceSize size, const VkBufferUsageFlags usage,
        const std::string_view name)
        : mDevice(&device)
        , mSize(std::max(size, VkDeviceSize{ 1 }))
        , mKind(kind)
        , mAddressable((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
    {
        const VkBufferCreateInfo create{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = mSize,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        mHandle = Owned<VkBuffer, vkDestroyBuffer>::make(device.getHandle(), vkCreateBuffer, create, "vkCreateBuffer");
        device.setName(mHandle.get(), name);

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device.getHandle(), mHandle.get(), &requirements);

        mMemory = device.getMemory().take(requirements, propertiesOf(kind), Tiling::Linear);
        checkVk(vkBindBufferMemory(device.getHandle(), mHandle.get(), mMemory.getHandle(), mMemory.getOffset()),
            "vkBindBufferMemory");

        if (mAddressable)
        {
            const VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .buffer = mHandle.get(),
            };
            mAddress = vkGetBufferDeviceAddress(device.getHandle(), &info);
        }
    }

    Buffer Buffer::make(const Device& device, const BufferKind kind, const VkDeviceSize size,
        const VkBufferUsageFlags usage, const std::string_view name)
    {
        return Buffer(device, kind, size, usage, name);
    }

    Buffer Buffer::deviceLocal(
        const Device& device, const VkDeviceSize size, const VkBufferUsageFlags usage, const std::string_view name)
    {
        return make(device, BufferKind::DeviceLocal, size, usage, name);
    }

    Buffer Buffer::hostWritten(
        const Device& device, const VkDeviceSize size, const VkBufferUsageFlags usage, const std::string_view name)
    {
        return make(device, BufferKind::HostWritten, size, usage, name);
    }

    Buffer Buffer::staging(
        const Device& device, const VkDeviceSize size, const VkBufferUsageFlags usage, const std::string_view name)
    {
        return make(device, BufferKind::Staging, size, usage, name);
    }

    bool Buffer::isIdle() const
    {
        return mDevice == nullptr || mRead.isIdle(mDevice->getTimeline());
    }

    void Buffer::waitIdle(const char* const what) const
    {
        if (mDevice != nullptr)
            mRead.waitIdle(mDevice->getTimeline(), what);
    }

    VkDeviceAddress Buffer::addressFor() const
    {
        nameFor(mDevice->getTimeline().getNext());
        return getDeviceAddress();
    }

    VkDescriptorBufferInfo Buffer::describe() const
    {
        nameFor(mDevice->getTimeline().getNext());
        return VkDescriptorBufferInfo{ mHandle.get(), 0, VK_WHOLE_SIZE };
    }

    Buffer growTo(Buffer& held, const Device& device, const BufferKind kind, const VkDeviceSize bytes,
        const VkBufferUsageFlags usage, const std::string_view name)
    {
        assert((held.isEmpty() || held.getKind() == kind) && "a table grown into another kind of memory");

        if (!held.isEmpty() && held.getSize() >= bytes)
            return Buffer();

        Buffer displaced = std::move(held);
        held = Buffer::make(device, kind, bytes, usage, name);

        return displaced;
    }

    VkBufferMemoryBarrier2 Buffer::describeBarrier(const BufferUse& from, const BufferUse& to) const
    {
        return VkBufferMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = from.mStage,
            .srcAccessMask = from.mAccess,
            .dstStageMask = to.mStage,
            .dstAccessMask = to.mAccess,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = mHandle.get(),
            .size = VK_WHOLE_SIZE,
        };
    }

    void Buffer::transition(VkCommandBuffer commands, const BufferUse& from, const BufferUse& to) const
    {
        Barriers barriers(commands);
        barriers.add(describeBarrier(from, to));
        barriers.flush();
    }

    void Buffer::clear(const VkCommandBuffer commands, const VkDeviceSize bytes) const
    {
        vkCmdFillBuffer(commands, mHandle.get(), 0, bytes, 0);
    }

    void Buffer::copyTo(const VkCommandBuffer commands, const Buffer& into, const VkDeviceSize bytes) const
    {
        assert(bytes <= mSize && bytes <= into.mSize && "a copy of more than either buffer holds");

        // Both ends, because a copy takes handles and an address names nothing: a host write over
        // either end while the copy is on the queue is the hazard `isIdle` is asked about.
        const std::uint64_t next = mDevice->getTimeline().getNext();
        nameFor(next);
        into.nameFor(next);

        const VkBufferCopy region{ .size = bytes };
        vkCmdCopyBuffer(commands, mHandle.get(), into.mHandle.get(), 1, &region);
    }

    void Buffer::orderForHostRead(VkCommandBuffer commands) const
    {
        assert(mKind == BufferKind::Staging && "a host-read dependency on memory nothing reads back");

        // Every way this renderer fills one: a copy out of an image, and a shader writing through
        // the buffer's own address. Naming both here rather than at each of the callers is what
        // stops one of them naming the wrong one.
        transition(commands,
            BufferUse{ VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                    | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
            Use::sBufferHostRead);
    }

    void Buffer::updateInline(
        VkCommandBuffer commands, const BufferUse& readers, const std::span<const std::byte> bytes) const
    {
        assert(bytes.size() <= mSize);

        // Both directions, because one buffer serves every frame: the write has to wait for the
        // last pass that read it and for the last write, and the next pass for the write.
        transition(commands,
            BufferUse{
                readers.mStage | Use::sBufferClearWrite.mStage, readers.mAccess | Use::sBufferClearWrite.mAccess },
            Use::sBufferClearWrite);

        vkCmdUpdateBuffer(commands, mHandle.get(), 0, bytes.size(), bytes.data());

        transition(commands, Use::sBufferClearWrite, readers);
    }
}

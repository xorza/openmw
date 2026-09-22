#include "buffer.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

#include "barriers.hpp"
#include "bufferusage.hpp"
#include "device.hpp"
#include "graveyard.hpp"
#include "physicaldevice.hpp"
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
                case BufferKind::ReadBack:
                    return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                        | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            }

            return 0;
        }

        /// The alignment a buffer's memory owes beyond what the driver asks for binding it: the
        /// rules Vulkan states at the use and not at the bind, which a driver's requirement need not
        /// cover. A scratch address must be a multiple of the scratch alignment, a shader binding
        /// table's base of the group base alignment, and a structure's offset of 256 — each written
        /// as an offset inside the buffer elsewhere, which is only enough where the buffer itself
        /// starts on the boundary. The allocator this fork had placed every range on a kilobyte,
        /// which covered all three by accident; the library places at what it is asked.
        VkDeviceSize alignmentOwedBy(const Device& device, const VkBufferUsageFlags usage)
        {
            VkDeviceSize owed = 1;
            if ((usage & sScratchUsage) == sScratchUsage)
                owed = std::max(owed, device.getPhysicalDevice().getStructureScratchAlignment());
            if ((usage & VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR) != 0)
                owed = std::max<VkDeviceSize>(
                    owed, device.getPhysicalDevice().getProperties().mRayTracingPipeline.shaderGroupBaseAlignment);
            if ((usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR) != 0)
                owed = std::max(owed, sStructureOffsetAlignment);

            return owed;
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
    }

    void Buffer::bind(DeviceMemory&& memory)
    {
        mMemory = std::move(memory);
        checkVk(vkBindBufferMemory(mDevice->getHandle(), mHandle.get(), mMemory.getHandle(), mMemory.getOffset()),
            "vkBindBufferMemory");

        if (mAddressable)
        {
            const VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .buffer = mHandle.get(),
            };
            mAddress = vkGetBufferDeviceAddress(mDevice->getHandle(), &info);
        }
    }

    Buffer::~Buffer()
    {
        assert(mayDestroy() && "a buffer destroyed while a submit still reads it; bury it");
    }

    Buffer& Buffer::operator=(Buffer&& other) noexcept
    {
        if (this != &other)
        {
            assert(mayDestroy() && "a buffer written over while a submit still reads it; replace it");

            mDevice = other.mDevice;
            mHandle = std::move(other.mHandle);
            mMemory = std::move(other.mMemory);
            mSize = other.mSize;
            mKind = other.mKind;
            mAddressable = other.mAddressable;
            mAddress = other.mAddress;
            mRead = other.mRead;
        }

        return *this;
    }

    bool Buffer::mayDestroy() const
    {
        return isEmpty() || isIdle();
    }

    Buffer Buffer::make(const Device& device, const BufferKind kind, const VkDeviceSize size,
        const VkBufferUsageFlags usage, const std::string_view name)
    {
        Buffer made(device, kind, size, usage, name);
        made.bind(device.getMemory().take(made.mHandle.get(), propertiesOf(kind), alignmentOwedBy(device, usage)));
        return made;
    }

    Result<Buffer, std::string_view> Buffer::tryMake(const MemoryUse use, const Device& device, const BufferKind kind,
        const VkDeviceSize size, const VkBufferUsageFlags usage, const std::string_view name)
    {
        Buffer made(device, kind, size, usage, name);
        Result<DeviceMemory, std::string_view> memory
            = device.getMemory().tryTake(made.mHandle.get(), propertiesOf(kind), alignmentOwedBy(device, usage), use);
        if (!memory.isOk())
            return Err{ memory.error() };

        made.bind(std::move(memory.value()));
        return made;
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

    Buffer Buffer::readBack(
        const Device& device, const VkDeviceSize size, const VkBufferUsageFlags usage, const std::string_view name)
    {
        return make(device, BufferKind::ReadBack, size, usage, name);
    }

    bool Buffer::isIdle() const
    {
        return mDevice == nullptr || mRead.isIdle(*mDevice);
    }

    void Buffer::waitIdle(const char* const what) const
    {
        if (mDevice != nullptr)
            mRead.waitIdle(*mDevice, what);
    }

    VkDeviceAddress Buffer::addressFor() const
    {
        assert(!isEmpty() && "an address of a buffer nobody made");

        nameForNext();
        return getDeviceAddress();
    }

    VkDescriptorBufferInfo Buffer::describe() const
    {
        assert(!isEmpty() && "a descriptor of a buffer nobody made");

        nameForNext();
        return VkDescriptorBufferInfo{ mHandle.get(), 0, VK_WHOLE_SIZE };
    }

    bool growTo(Buffer& held, const Device& device, const BufferKind kind, const VkDeviceSize bytes,
        const VkBufferUsageFlags usage, const std::string_view name)
    {
        assert((held.isEmpty() || held.getKind() == kind) && "a table grown into another kind of memory");

        if (!held.isEmpty() && held.getSize() >= bytes)
            return false;

        device.getGraveyard().replace(held, Buffer::make(device, kind, bytes, usage, name));
        return true;
    }

    bool outgrow(Buffer& held, const Device& device, const BufferKind kind, const VkDeviceSize bytes,
        const VkBufferUsageFlags usage, const std::string_view name)
    {
        if (!held.isEmpty() && held.getSize() >= bytes)
            return false;

        return growTo(held, device, kind, std::max(bytes, held.getSize() * 2), usage, name);
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
        assert(!isEmpty() && "a barrier on a buffer nobody made");

        Barriers barriers(commands);
        barriers.add(describeBarrier(from, to));
        barriers.flush();
    }

    void Buffer::clear(const VkCommandBuffer commands, const VkDeviceSize bytes) const
    {
        assert(!isEmpty() && "a clear of a buffer nobody made");

        vkCmdFillBuffer(commands, mHandle.get(), 0, bytes, 0);
    }

    void Buffer::copyTo(const VkCommandBuffer commands, const Buffer& into, const VkDeviceSize bytes) const
    {
        assert(!isEmpty() && "a copy out of a buffer nobody made");

        assert(bytes <= mSize && bytes <= into.mSize && "a copy of more than either buffer holds");

        // Both ends, because a copy takes handles and an address names nothing: a host write over
        // either end while the copy is on the queue is the hazard `isIdle` is asked about.
        nameForNext();
        into.nameForNext();

        const VkBufferCopy region{ .size = bytes };
        vkCmdCopyBuffer(commands, mHandle.get(), into.mHandle.get(), 1, &region);
    }

    void Buffer::orderForHostRead(VkCommandBuffer commands) const
    {
        assert(!isEmpty() && "a host read of a buffer nobody made");

        assert(mKind == BufferKind::ReadBack && "a host-read dependency on memory nothing reads back");

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
        assert(!isEmpty() && "a write into a buffer nobody made");

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

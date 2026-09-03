#include "graveyard.hpp"

#include <algorithm>
#include <utility>

#include "commands.hpp"
#include "device.hpp"

namespace Rtx
{
    bool outgrow(Buffer& held, const Device& device, const VkDeviceSize bytes, const VkBufferUsageFlags usage,
        Graveyard& graveyard)
    {
        if (held.getSize() >= bytes)
            return false;

        graveyard.bury(growTo(held, device, std::max(bytes, held.getSize() * 2), usage));
        return true;
    }

    Graveyard::Graveyard(const Device& device, CommandPool& pool)
        : mDevice(device)
        , mPool(pool)
    {
    }

    Graveyard::~Graveyard()
    {
        clear();
    }

    void Graveyard::bury(Buffer&& buffer)
    {
        if (buffer.getHandle() != VK_NULL_HANDLE)
            mBuffers.push_back(std::move(buffer));
    }

    void Graveyard::bury(Texture&& texture)
    {
        if (texture.getView() != VK_NULL_HANDLE)
            mTextures.push_back(std::move(texture));
    }

    void Graveyard::bury(VkAccelerationStructureKHR structure)
    {
        if (structure != VK_NULL_HANDLE)
            mStructures.push_back(structure);
    }

    void Graveyard::bury(VkMicromapEXT micromap)
    {
        if (micromap != VK_NULL_HANDLE)
            mMicromaps.push_back(micromap);
    }

    void Graveyard::bury(VkDescriptorPool pool)
    {
        if (pool != VK_NULL_HANDLE)
            mPools.push_back(pool);
    }

    void Graveyard::bury(StructureStorage& storage, const StructureRoom& room)
    {
        if (!room.empty())
            mRooms.push_back(Room{ .mStorage = &storage, .mRoom = room });
    }

    void Graveyard::bury(VkCommandBuffer commands)
    {
        if (commands != VK_NULL_HANDLE)
            mCommands.push_back(commands);
    }

    void Graveyard::bury(std::unique_ptr<Image>&& image)
    {
        if (image != nullptr)
            mImages.push_back(std::move(image));
    }

    void Graveyard::clear()
    {
        const DeviceFunctions& functions = mDevice.getFunctions();

        // The structures before the micromaps they reference, and both before the rooms they stand
        // in: a room given back is the next structure's, and one given back under a structure still
        // standing is two of them in one place.
        for (const VkAccelerationStructureKHR structure : mStructures)
            functions.mDestroyAccelerationStructure(mDevice.getHandle(), structure, nullptr);
        for (const VkMicromapEXT micromap : mMicromaps)
            functions.mDestroyMicromap(mDevice.getHandle(), micromap, nullptr);
        for (const Room& room : mRooms)
            room.mStorage->give(room.mRoom);
        for (const VkDescriptorPool pool : mPools)
            vkDestroyDescriptorPool(mDevice.getHandle(), pool, nullptr);

        mStructures.clear();
        mMicromaps.clear();
        mRooms.clear();
        mPools.clear();
        mBuffers.clear();
        mTextures.clear();
        mImages.clear();

        mPool.free(mCommands);
        mCommands.clear();
    }
}

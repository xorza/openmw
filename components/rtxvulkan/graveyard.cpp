#include "graveyard.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

#include "commands.hpp"
#include "device.hpp"
#include "timeline.hpp"

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

    std::uint64_t Graveyard::stamp() const
    {
        return mDevice.getTimeline().getNext();
    }

    void Graveyard::bury(Buffer&& buffer)
    {
        if (buffer.getHandle() != VK_NULL_HANDLE)
            mBuffers.push_back({ stamp(), std::move(buffer) });
    }

    void Graveyard::bury(Texture&& texture)
    {
        if (texture.getView() != VK_NULL_HANDLE)
            mTextures.push_back({ stamp(), std::move(texture) });
    }

    void Graveyard::bury(VkAccelerationStructureKHR structure)
    {
        if (structure != VK_NULL_HANDLE)
            mStructures.push_back({ stamp(), structure });
    }

    void Graveyard::bury(VkDescriptorPool pool)
    {
        if (pool != VK_NULL_HANDLE)
            mPools.push_back({ stamp(), pool });
    }

    void Graveyard::bury(VkQueryPool pool)
    {
        if (pool != VK_NULL_HANDLE)
            mQueryPools.push_back({ stamp(), pool });
    }

    void Graveyard::bury(StructureStorage& storage, const StructureRoom& room)
    {
        if (!room.empty())
            mRooms.push_back({ stamp(), Room{ .mStorage = &storage, .mRoom = room } });
    }

    void Graveyard::bury(VkCommandBuffer commands)
    {
        if (commands != VK_NULL_HANDLE)
            mCommands.push_back({ stamp(), commands });
    }

    void Graveyard::bury(std::unique_ptr<Image>&& image)
    {
        if (image != nullptr)
            mImages.push_back({ stamp(), std::move(image) });
    }

    template <class T, class Destroy>
    void Graveyard::free(std::vector<Held<T>>& held, const std::uint64_t finished, Destroy&& destroy)
    {
        const auto kept = std::find_if(
            held.begin(), held.end(), [finished](const Held<T>& each) { return each.mUntil > finished; });
        for (auto at = held.begin(); at != kept; ++at)
            destroy(at->mObject);

        held.erase(held.begin(), kept);
    }

    void Graveyard::collect()
    {
        freeThrough(mDevice.getTimeline().getKnownFinished());
    }

    void Graveyard::clear()
    {
        freeThrough(std::numeric_limits<std::uint64_t>::max());
    }

    void Graveyard::freeThrough(const std::uint64_t finished)
    {
        const DeviceFunctions& functions = mDevice.getFunctions();

        // The structures before the rooms they stand in: a room given back is the next
        // structure's, and one given back under a structure still standing is two of them in one
        // place.
        free(mStructures, finished, [&](const VkAccelerationStructureKHR structure) {
            functions.mDestroyAccelerationStructure(mDevice.getHandle(), structure, nullptr);
        });
        free(mRooms, finished, [](const Room& room) { room.mStorage->give(room.mRoom); });
        free(mPools, finished,
            [&](const VkDescriptorPool pool) { vkDestroyDescriptorPool(mDevice.getHandle(), pool, nullptr); });
        free(mQueryPools, finished,
            [&](const VkQueryPool pool) { vkDestroyQueryPool(mDevice.getHandle(), pool, nullptr); });
        free(mBuffers, finished, [](Buffer& buffer) { buffer = Buffer(); });
        free(mTextures, finished, [](Texture& texture) { texture = Texture(); });
        free(mImages, finished, [](std::unique_ptr<Image>& image) { image.reset(); });
        free(mCommands, finished,
            [&](const VkCommandBuffer commands) { mPool.free(std::span<const VkCommandBuffer>(&commands, 1)); });
    }
}

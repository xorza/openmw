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
    bool outgrow(Buffer& held, const Device& device, const BufferKind kind, const VkDeviceSize bytes,
        const VkBufferUsageFlags usage, const std::string_view name, Graveyard& graveyard)
    {
        if (!held.isEmpty() && held.getSize() >= bytes)
            return false;

        graveyard.bury(growTo(held, device, kind, std::max(bytes, held.getSize() * 2), usage, name));
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
        if (!texture.isEmpty())
            mTextures.push_back({ stamp(), std::move(texture) });
    }

    void Graveyard::bury(AccelerationStructure&& structure)
    {
        if (!structure.isEmpty())
            mStructures.push_back({ stamp(), std::move(structure) });
    }

    void Graveyard::bury(VkQueryPool pool)
    {
        if (pool != VK_NULL_HANDLE)
            mQueryPools.push_back({ stamp(), pool });
    }

    void Graveyard::bury(VkCommandBuffer commands)
    {
        if (commands != VK_NULL_HANDLE)
            mCommands.push_back({ stamp(), commands });
    }

    void Graveyard::bury(Image&& image)
    {
        if (!image.isEmpty())
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
        free(mStructures, finished, [](AccelerationStructure& structure) { structure = AccelerationStructure(); });
        free(mQueryPools, finished,
            [&](const VkQueryPool pool) { vkDestroyQueryPool(mDevice.getHandle(), pool, nullptr); });
        free(mBuffers, finished, [](Buffer& buffer) { buffer = Buffer(); });
        free(mTextures, finished, [](Texture& texture) { texture = Texture(); });
        free(mImages, finished, [](Image& image) { image = Image(); });
        free(mCommands, finished,
            [&](const VkCommandBuffer commands) { mPool.free(std::span<const VkCommandBuffer>(&commands, 1)); });
    }
}

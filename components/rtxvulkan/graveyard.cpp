#include "graveyard.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "commands.hpp"
#include "device.hpp"
#include "timeline.hpp"

namespace Rtx
{
    Graveyard::Graveyard(const Device& device)
        : mDevice(device)
    {
    }

    Graveyard::~Graveyard()
    {
        // What the device's own idle left: burials stamped for a submit nobody will make now.
        collectIdle();
    }

    std::uint64_t Graveyard::stamp() const
    {
        return mDevice.getTimeline().getNext();
    }

    void Graveyard::bury(Buffer&& buffer)
    {
        if (!buffer.isEmpty())
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

    void Graveyard::bury(QueryPool&& pool)
    {
        if (pool.get() != VK_NULL_HANDLE)
            mQueryPools.push_back({ stamp(), std::move(pool) });
    }

    void Graveyard::bury(std::shared_ptr<void>&& held)
    {
        if (held != nullptr)
            mOthers.push_back({ stamp(), std::move(held) });
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

    template <class T>
    void Graveyard::free(std::vector<Held<T>>& held, const std::uint64_t finished)
    {
        free(held, finished, [](T& object) { object = T(); });
    }

    void Graveyard::collect()
    {
        freeThrough(mDevice.getTimeline().getKnownFinished());
    }

    void Graveyard::collectIdle()
    {
        assert(mDevice.getTimeline().isIdle() && "everything held destroyed under a submit still on the queue");
        assert(!mDevice.getPool().hasDeferred() && "everything held destroyed under a batch not yet submitted");

        freeThrough(std::numeric_limits<std::uint64_t>::max());
    }

    void Graveyard::freeThrough(const std::uint64_t finished)
    {
        // Set for the whole sweep and not per object, because a scene freed last destroys every
        // structure and texture it still holds on its way out.
        mReaping = true;

        free(mStructures, finished);
        free(mQueryPools, finished);
        free(mBuffers, finished);
        free(mTextures, finished);
        free(mImages, finished);
        free(mCommands, finished, [&](const VkCommandBuffer commands) {
            mDevice.getPool().recycle(std::span<const VkCommandBuffer>(&commands, 1));
        });
        free(mOthers, finished);

        mReaping = false;
    }
}

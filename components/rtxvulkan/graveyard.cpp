#include "graveyard.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <utility>

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
            mBuffers.hold(stamp(), std::move(buffer));
    }

    void Graveyard::bury(Texture&& texture)
    {
        if (!texture.isEmpty())
            mTextures.hold(stamp(), std::move(texture));
    }

    void Graveyard::bury(AccelerationStructure&& structure)
    {
        if (!structure.isEmpty())
            mStructures.hold(stamp(), std::move(structure));
    }

    void Graveyard::bury(QueryPool&& pool)
    {
        if (pool.get() != VK_NULL_HANDLE)
            mQueryPools.hold(stamp(), std::move(pool));
    }

    void Graveyard::bury(std::shared_ptr<void>&& held)
    {
        if (held != nullptr)
            mOthers.hold(stamp(), std::move(held));
    }

    void Graveyard::bury(Image&& image)
    {
        if (!image.isEmpty())
            mImages.hold(stamp(), std::move(image));
    }

    template <class T>
    void Graveyard::free(Retiring<T>& held, const std::uint64_t finished)
    {
        // Written over with an empty one, which is how every object here destroys itself — and
        // asserts, against its own stamp, that nothing on the queue still reads it. The stamp is
        // at or before the burial's, so the assert fires exactly where a buried object was named
        // again after its burial.
        held.releaseThrough(finished, [](T& object) { object = T(); });
    }

    void Graveyard::collect()
    {
        freeThrough(mDevice.getTimeline().getKnownFinished());
    }

    void Graveyard::collectIdle()
    {
        assert(mDevice.getTimeline().isIdle() && "everything held destroyed under a submit still on the queue");

        freeThrough(std::numeric_limits<std::uint64_t>::max());
    }

    void Graveyard::freeThrough(const std::uint64_t finished)
    {
        free(mStructures, finished);
        free(mQueryPools, finished);
        free(mBuffers, finished);
        free(mTextures, finished);
        free(mImages, finished);
        free(mOthers, finished);
    }
}

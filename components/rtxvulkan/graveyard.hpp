#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <vulkan/vulkan_core.h>

#include "accelerationstructure.hpp"
#include "buffer.hpp"
#include "handles.hpp"
#include "image.hpp"
#include "retiring.hpp"
#include "texture.hpp"

namespace Rtx
{
    class Device;

    /// What a submit the queue may not have reached yet may still be reading, held until the
    /// timeline says it has run: everything on the frame path buries what it is finished with here.
    /// One for the queue, the device's own, and each burial is stamped with the value of the next
    /// submit — which is after every submit already made and is the one a deferred batch rides —
    /// so an object cannot be freed before its last reader, whoever buried it and whatever frame
    /// was recording. Two graveyards keyed by frame slot were the alternative, and a burial in the
    /// wrong one was a device lost with an invalid read.
    class Graveyard
    {
    public:
        explicit Graveyard(const Device& device);
        ~Graveyard();

        /// Each takes an empty one and does nothing with it.
        void bury(Buffer&& buffer);
        void bury(Texture&& texture);
        void bury(Image&& image);

        /// A structure and the room it stands in, given back together once nothing can be built
        /// or traced in it.
        void bury(AccelerationStructure&& structure);

        /// A query pool a batch in flight may still be writing answers into.
        void bury(QueryPool&& pool);

        /// Anything else a submit may still read, held by ownership and let go of last — after
        /// every structure, because what is buried this way is a scene, and a structure gives its
        /// room back to a storage the scene owns. Type-erased, so this file names no scene.
        void bury(std::shared_ptr<void>&& held);

        /// Puts `made` where `held` stands and buries what stood there: the one way a table, a
        /// texture or a structure is replaced, so the assignment that replaces it cannot destroy
        /// what a frame in flight reads.
        template <class T>
        void replace(T& held, T&& made)
        {
            bury(std::exchange(held, std::move(made)));
        }

        /// Destroys what the timeline has been seen to pass. Asked by the device after every
        /// wait, which is where what the timeline knows changes, and by nothing else.
        void collect();

        /// Destroys everything held, a burial stamped for a submit nobody has made included: for
        /// a queue nothing is on and nothing is recorded for, which a drain and the teardown are.
        /// The timeline idle is asserted rather than trusted, because the same call one wait too
        /// early is a destroyed object under a submit; that nothing is recorded is the caller's,
        /// through `Device::collectIdle`.
        void collectIdle();

        /// Whether this is in the middle of freeing what the timeline has passed — the one moment a
        /// device object a submit has read is destroyed with the queue still running, and so the
        /// one `Device::mayDestroy` allows.
        bool isReaping() const { return mReaping; }

        // Read by the tests and by nothing else.
        std::size_t getHeldCount() const
        {
            return mBuffers.size() + mTextures.size() + mImages.size() + mStructures.size() + mQueryPools.size()
                + mOthers.size();
        }

    private:
        std::uint64_t stamp() const;

        /// Destroys everything stamped at or below `finished`, in the order the destructors need.
        void freeThrough(std::uint64_t finished);

        /// Destroys the prefix of `held` stamped at or below `finished`: written over with an
        /// empty one, which is how every object here destroys itself.
        template <class T>
        static void free(Retiring<T>& held, std::uint64_t finished);

        const Device& mDevice;

        bool mReaping = false;

        Retiring<Buffer> mBuffers;
        Retiring<Texture> mTextures;
        Retiring<Image> mImages;
        Retiring<AccelerationStructure> mStructures;
        Retiring<QueryPool> mQueryPools;
        Retiring<std::shared_ptr<void>> mOthers;
    };
}

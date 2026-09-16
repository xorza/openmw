#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "accelerationstructure.hpp"
#include "buffer.hpp"
#include "handles.hpp"
#include "image.hpp"
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

        /// A one-shot command buffer the pool handed out, freed once it has run.
        void bury(VkCommandBuffer commands);

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

        /// Destroys what the timeline has been seen to pass. Asked by every wait, which is where
        /// what the timeline knows changes, and by nothing else.
        void collect();

        /// Destroys everything held, a burial stamped for a submit nobody has made included: for
        /// a queue nothing is on and nothing is recorded for, which a drain and the teardown are.
        /// Asserted rather than trusted — the timeline idle and the pool holding nothing deferred
        /// — because the same call one wait too early is a destroyed object under a submit.
        void collectIdle();

        // Read by the tests and by nothing else.
        std::size_t getHeldCount() const
        {
            return mBuffers.size() + mTextures.size() + mImages.size() + mStructures.size() + mQueryPools.size()
                + mCommands.size() + mOthers.size();
        }

    private:
        /// One thing buried and the value it is held until. In burial order, which is stamp
        /// order, so what `collect` frees is a prefix.
        template <class T>
        struct Held
        {
            std::uint64_t mUntil = 0;
            T mObject;
        };

        std::uint64_t stamp() const;

        /// Destroys everything stamped at or below `finished`, in the order the destructors need.
        void freeThrough(std::uint64_t finished);

        /// Destroys the prefix of `held` stamped at or below `finished`, with `destroy` on each.
        template <class T, class Destroy>
        static void free(std::vector<Held<T>>& held, std::uint64_t finished, Destroy&& destroy);

        /// The same, for what destroys itself when written over.
        template <class T>
        static void free(std::vector<Held<T>>& held, std::uint64_t finished);

        const Device& mDevice;

        // Cleared and refilled, never freed: a frame path does not allocate, and what a frame
        // buries settles at the busiest frame so far.
        std::vector<Held<Buffer>> mBuffers;
        std::vector<Held<Texture>> mTextures;
        std::vector<Held<Image>> mImages;
        std::vector<Held<AccelerationStructure>> mStructures;
        std::vector<Held<QueryPool>> mQueryPools;
        std::vector<Held<VkCommandBuffer>> mCommands;
        std::vector<Held<std::shared_ptr<void>>> mOthers;
    };
}

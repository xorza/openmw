#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "accelerationstructure.hpp"
#include "buffer.hpp"
#include "image.hpp"
#include "texture.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;

    /// What a submit the queue may not have reached yet may still be reading, held until the
    /// timeline says it has run: everything on the frame path buries what it is finished with here.
    /// One for the renderer, and each burial is stamped with the value of the next submit — which
    /// is after every submit already made and is the one a deferred batch rides — so an object
    /// cannot be freed before its last reader, whoever buried it and whatever frame was recording.
    /// Two graveyards keyed by frame slot were the alternative, and a burial in the wrong one was
    /// a device lost with an invalid read.
    class Graveyard
    {
    public:
        Graveyard(const Device& device, CommandPool& pool);
        ~Graveyard();

        /// Each takes an empty one and does nothing with it, so a caller can bury what a growth
        /// displaced without asking whether it displaced anything.
        void bury(Buffer&& buffer);
        void bury(Texture&& texture);
        void bury(Image&& image);

        /// A structure and the room it stands in, given back together once nothing can be built
        /// or traced in it.
        void bury(AccelerationStructure&& structure);

        /// A query pool a batch in flight may still be writing answers into.
        void bury(VkQueryPool pool);

        /// A one-shot command buffer the pool handed out, freed once it has run.
        void bury(VkCommandBuffer commands);

        /// Destroys what the timeline has been seen to pass — asked once per wait, which is where
        /// what it knows changes.
        void collect();

        /// Destroys everything held, whatever the timeline says. After a device idle and never
        /// before: the caller is what knows.
        void clear();

        // Read by the tests and by nothing else.
        std::size_t getHeldCount() const
        {
            return mBuffers.size() + mTextures.size() + mImages.size() + mStructures.size() + mQueryPools.size()
                + mCommands.size();
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

        /// Destroys everything stamped at or below `finished`, in the order the destructors
        /// need: what `collect` and `clear` share, with the value the two differ by.
        void freeThrough(std::uint64_t finished);

        /// Destroys the prefix of `held` stamped at or below `finished`, with `destroy` on each.
        template <class T, class Destroy>
        static void free(std::vector<Held<T>>& held, std::uint64_t finished, Destroy&& destroy);

        const Device& mDevice;
        CommandPool& mPool;

        // Cleared and refilled, never freed: a frame path does not allocate, and what a frame
        // buries settles at the busiest frame so far.
        std::vector<Held<Buffer>> mBuffers;
        std::vector<Held<Texture>> mTextures;
        std::vector<Held<Image>> mImages;
        std::vector<Held<AccelerationStructure>> mStructures;
        std::vector<Held<VkQueryPool>> mQueryPools;
        std::vector<Held<VkCommandBuffer>> mCommands;
    };

    /// `growTo` for a table that keeps growing: makes `held` able to hold `bytes`, at twice what it
    /// holds where that is more, so the table is made again a logarithmic number of times rather
    /// than once per arrival — and buries what that displaced. True where it was made again, which
    /// is a table holding nothing that the caller has to fill whole.
    bool outgrow(Buffer& held, const Device& device, BufferKind kind, VkDeviceSize bytes, VkBufferUsageFlags usage,
        std::string_view name, Graveyard& graveyard);
}

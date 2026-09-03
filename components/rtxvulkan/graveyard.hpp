#pragma once

#include <memory>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "buffer.hpp"
#include "image.hpp"
#include "structurestorage.hpp"
#include "texture.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;

    /// What a frame in flight may still be reading, held until its fence says it has stopped.
    ///
    /// **Two frames in flight means nothing is destroyed when it is let go.** A texture the scene
    /// dropped, a structure a departed mesh gave back, a table made again because it grew, the
    /// staging an upload read from — each is named by a command buffer the queue may not have
    /// reached yet, and destroying it there is a use after free the driver may or may not survive.
    /// So everything on the frame path buries what it is finished with here, under the frame that
    /// is being recorded, and that frame's fence is what empties it: by the time it has signalled,
    /// every earlier submit on the queue has finished too.
    class Graveyard
    {
    public:
        Graveyard(const Device& device, CommandPool& pool);
        ~Graveyard();

        Graveyard(const Graveyard&) = delete;
        Graveyard& operator=(const Graveyard&) = delete;

        /// Each takes an empty one and does nothing with it, so a caller can bury what a growth
        /// displaced without asking whether it displaced anything.
        void bury(Buffer&& buffer);
        void bury(Texture&& texture);

        /// By pointer where the others are by value, because `Image` is not movable: what owns one
        /// owns it through a `unique_ptr` and hands that over.
        void bury(std::unique_ptr<Image>&& image);
        void bury(VkAccelerationStructureKHR structure);

        /// A micromap, which goes after every structure buried beside it: a structure references
        /// the micromap it was built with for as long as it is traced.
        void bury(VkMicromapEXT micromap);

        /// A descriptor pool and every set allocated from it, which a dispatch in flight may still
        /// be reading through.
        void bury(VkDescriptorPool pool);

        /// A room in `storage`, given back once nothing can be built or traced in it.
        void bury(StructureStorage& storage, const StructureRoom& room);

        /// A one-shot command buffer the pool handed out, freed once it has run.
        void bury(VkCommandBuffer commands);

        /// Destroys everything held. **After the fence and never before**: the caller is what knows.
        void clear();

    private:
        struct Room
        {
            StructureStorage* mStorage = nullptr;
            StructureRoom mRoom;
        };

        const Device& mDevice;
        CommandPool& mPool;

        // Cleared and refilled, never freed: a frame path does not allocate, and what a frame
        // buries settles at the busiest frame so far.
        std::vector<Buffer> mBuffers;
        std::vector<Texture> mTextures;
        std::vector<std::unique_ptr<Image>> mImages;
        std::vector<VkAccelerationStructureKHR> mStructures;
        std::vector<VkMicromapEXT> mMicromaps;
        std::vector<VkDescriptorPool> mPools;
        std::vector<Room> mRooms;
        std::vector<VkCommandBuffer> mCommands;
    };

    /// `growTo` for a table that keeps growing: makes `held` able to hold `bytes`, at twice what it
    /// holds where that is more, so the table is made again a logarithmic number of times rather
    /// than once per arrival — and buries what that displaced. True where it was made again, which
    /// is a table holding nothing that the caller has to fill whole.
    bool outgrow(
        Buffer& held, const Device& device, VkDeviceSize bytes, VkBufferUsageFlags usage, Graveyard& graveyard);
}

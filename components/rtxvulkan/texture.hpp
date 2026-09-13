#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/slots.hpp>
#include <components/rtx/texturedata.hpp>

#include "frameslots.hpp"
#include "handles.hpp"
#include "image.hpp"
#include "owned.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class Graveyard;

    /// A sampled image on the GPU, the levels a content file brought for it, and the light the
    /// file already had painted into it — two `Image`s and what was uploaded into them. The second
    /// is the shading map, `SHADING_EXTENT` squared, which travels with the texture because it is
    /// measured on it and read at its coordinates.
    class Texture
    {
    public:
        /// A slot with nothing in it yet, which is what the array holds while it is being filled.
        Texture() = default;

        /// @param name what a capture calls it. Empty where the build names no objects.
        /// @param regions the caller's scratch, cleared and refilled here with one copy per level.
        Texture(const Device& device, Batch& batch, const TextureData& data, std::string_view name,
            std::vector<VkBufferImageCopy>& regions);
        Texture(Texture&&) noexcept = default;
        Texture& operator=(Texture&&) noexcept = default;

        /// What a sampler reads, or nothing where the slot holds no texture.
        VkImageView getView() const { return mImage == nullptr ? VK_NULL_HANDLE : mImage->getView(); }

        /// The shading map beside it, likewise.
        VkImageView getShadingView() const { return mShading == nullptr ? VK_NULL_HANDLE : mShading->getView(); }

        /// The size of the data uploaded, the map's included, which for a block-compressed image is
        /// what it occupies.
        VkDeviceSize getBytes() const { return mBytes; }

    private:
        /// By pointer, because `Image` is not movable and a texture is: the array holds them in
        /// a vector, and a slot given back is buried under the frame that may still be reading it.
        std::unique_ptr<Image> mImage;
        std::unique_ptr<Image> mShading;

        VkDeviceSize mBytes = 0;
    };

    /// What a texture array stands: how many of its slots hold a texture, and what those come to,
    /// out of one walk, so the two cannot disagree about which slots they counted.
    struct TexturesHeld
    {
        std::uint32_t mCount = 0;
        VkDeviceSize mBytes = 0;
    };

    /// Every texture a scene uses, in one descriptor array a shader indexes by material, and every
    /// texture's shading map in a second array beside it at the same slot. The maps are an array
    /// and not a buffer, because a map is a grid the texture unit filters; an array of their own
    /// for the reason `texturearray.glsl` gives.
    ///
    /// **One set per frame in flight, and a debt per set**, the way `SlotTable` keeps its copies:
    /// an arrival writes the slots it brought into the set the next placement binds and owes them
    /// to the other, which is paid when that set's frame comes round. Update after bind is what
    /// makes writing a set legal while a command that bound it is on the queue — for a slot that
    /// command does not read. A slot the sweep freed and an arrival took over is one the frame
    /// behind is still reading through its material table, and one set written from the host
    /// while it traced was exactly that read.
    class TextureArray
    {
    public:
        /// An array of `slots` textures, with `textures` written into the slots they name. The
        /// length is the scene's table and not what was described, because a slot the scene has
        /// given back still sits between two that are. `textures` may be empty: a slot nothing
        /// describes is one no material names, which is what `descriptorBindingPartiallyBound` is
        /// required for.
        TextureArray(const Device& device, Batch& batch, std::uint32_t slots, std::span<const TextureData> textures,
            Graveyard& graveyard);

        /// Uploads each of `arrived` into the slot it names, leaving every other texture alone —
        /// why the sets are allocated at the maximum rather than at the scene's count. By slot and
        /// not by appending, because a slot a departing cell freed is taken over wherever it sits.
        /// What a slot held before goes to `graveyard`: a frame in flight may be reading it. The
        /// descriptors are owed to every set and written by `sync`.
        void write(Batch& batch, std::span<const TextureData> arrived, Graveyard& graveyard);

        /// Writes the descriptors `slot`'s set owes. Before the placement that binds it, after the
        /// ring has waited out the frame that last read it.
        void sync(FrameSlot slot);

        /// Destroys the images of `slots`, leaving the slots themselves where they are. The
        /// descriptors are left naming what has gone, which `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT`
        /// makes legal: no live material names a freed slot. The array does not shrink, because the
        /// scene's table has not either.
        void drop(std::span<const std::uint32_t> slots, Graveyard& graveyard);

        VkDescriptorSetLayout getLayout() const { return mLayout.get(); }

        /// The set `slot`'s frame binds, which `sync(slot)` brought up to date.
        VkDescriptorSet getSet(FrameSlot slot) const
        {
            assert(slot.get() < sFrameSlots);
            return mSets[slot.get()].mSet;
        }

        /// How long the array is, which is where an append begins and what an uploader compares a
        /// scene's table against. Not how many textures there are: see `getHeld`.
        std::uint32_t getCount() const { return static_cast<std::uint32_t>(mTextures.size()); }

        /// What the array actually stands. A slot the scene gave back holds nothing and costs
        /// nothing, and neither is counted here.
        TexturesHeld getHeld() const;

    private:
        /// A descriptor set and the pool it was taken from, which is what frees it.
        struct SetPool
        {
            Owned<VkDescriptorPool, vkDestroyDescriptorPool> mPool;
            VkDescriptorSet mSet = VK_NULL_HANDLE;
        };

        /// Queues a write of `view` into `set` at `binding[slot]`, behind the image info the write
        /// names by address.
        void queueWrite(VkDescriptorSet set, std::uint32_t binding, std::uint32_t slot, VkImageView view,
            std::vector<VkDescriptorImageInfo>& images, std::vector<VkWriteDescriptorSet>& writes) const;

        /// Grows the array to reach `slot`, and refuses one past what the binding holds.
        void reserveSlot(std::uint32_t slot);

        const Device& mDevice;

        /// Cleared and refilled by every describe and every write, never freed. Each settles at the
        /// busiest arrival so far, and an arrival is the frame with the least room to grow one.
        std::vector<VkDescriptorImageInfo> mImageScratch;
        std::vector<VkWriteDescriptorSet> mWriteScratch;
        std::vector<VkBufferImageCopy> mRegionScratch;

        /// Indexed by slot. A slot the scene has freed holds nothing until something takes it over —
        /// `drop` buries the image it had, and the descriptor is left naming what has gone for the
        /// reason `drop` gives.
        std::vector<Texture> mTextures;

        Sampler mSampler;
        SetLayout mLayout;
        std::array<SetPool, sFrameSlots> mSets;

        /// The slots each set has yet to be told, each once however often it was written.
        std::array<SlotSet, sFrameSlots> mOwed;
    };
}

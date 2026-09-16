#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/slots.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/texturewrap.hpp>

#include "descriptorsets.hpp"
#include "frameslots.hpp"
#include "handles.hpp"
#include "image.hpp"
#include "readstamp.hpp"

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

        /// Whether the slot holds no texture.
        bool isEmpty() const { return mImage.isEmpty(); }

        /// The texture and its shading map as a sampled descriptor takes them, through `sampler`,
        /// from the read-only layout an upload leaves them in.
        VkDescriptorImageInfo describe(VkSampler sampler) const
        {
            return mImage.describeSampled(sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        VkDescriptorImageInfo describeShading(VkSampler sampler) const
        {
            return mShading.describeSampled(sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        // Read by the tests and by nothing else: the interface's pass draws with one.
        VkImageView getView() const { return mImage.getView(); }

        /// How the file said this is addressed past its edges, which picks the sampler the array
        /// binds it through.
        TextureWrap getWrap() const { return mWrap; }

        /// The size of the data uploaded, the map's included, which for a block-compressed image is
        /// what it occupies.
        VkDeviceSize getBytes() const { return mBytes; }

    private:
        Image mImage;
        Image mShading;

        TextureWrap mWrap = TextureWrap::Repeat;
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
        ///
        /// @param layout what `describeLayout` made: every array is shaped by the one the renderer
        ///        keeps, which is what lets one pass be handed any scene's set.
        TextureArray(const Device& device, Graveyard& graveyard, Batch& batch, const SetLayout& layout,
            std::uint32_t slots, std::span<const TextureData> textures);

        /// The shape of every set an array here holds: two bindless arrays, partially bound and
        /// updated after bind. Made once by whoever owns the passes that name it.
        static SetLayout describeLayout(const Device& device);

        /// Uploads each of `arrived` into the slot it names, leaving every other texture alone —
        /// why the sets are allocated at the maximum rather than at the scene's count. By slot and
        /// not by appending, because a slot a departing cell freed is taken over wherever it sits.
        /// What a slot held before goes to `graveyard`: a frame in flight may be reading it. The
        /// descriptors are owed to every set and written by `sync`.
        void write(Batch& batch, std::span<const TextureData> arrived);

        /// Writes the descriptors `slot`'s set owes. Before the placement that binds it, after
        /// `finishReads`: the bindings allow an update after a bind, but not of a descriptor a
        /// pending submit samples, and a trace samples whichever slots its materials name.
        void sync(FrameSlot slot);

        /// Waits until nothing on the queue binds `slot`'s set, ahead of the `sync` that writes it.
        void finishReads(FrameSlot slot) const;

        /// Destroys the images of `slots`, leaving the slots themselves where they are. The
        /// descriptors are left naming what has gone, which `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT`
        /// makes legal: no live material names a freed slot. The array does not shrink, because the
        /// scene's table has not either.
        void drop(std::span<const std::uint32_t> slots);

        /// The set `slot`'s frame binds, which `sync(slot)` brought up to date. A hand-out, so it
        /// names the set for the next submit the way `Buffer::addressFor` names a buffer.
        VkDescriptorSet getSet(FrameSlot slot) const;

        /// How long the array is, which is where an append begins and what an uploader compares a
        /// scene's table against. Not how many textures there are: see `getHeld`.
        std::uint32_t getCount() const { return static_cast<std::uint32_t>(mTextures.size()); }

        /// What the array actually stands. A slot the scene gave back holds nothing and costs
        /// nothing, and neither is counted here.
        TexturesHeld getHeld() const;

    private:
        /// Queues a write of `image` into `set` at `binding[slot]`, behind the image info the write
        /// names by address.
        static void queueWrite(VkDescriptorSet set, std::uint32_t binding, std::uint32_t slot,
            const VkDescriptorImageInfo& image, std::vector<VkDescriptorImageInfo>& images,
            std::vector<VkWriteDescriptorSet>& writes);

        /// Grows the array to reach `slot`, and refuses one past what the binding holds.
        void reserveSlot(std::uint32_t slot);

        const Device& mDevice;

        /// Where a texture goes when its slot is written over or dropped: a frame in flight may
        /// still sample it.
        Graveyard& mGraveyard;

        /// Cleared and refilled by every describe and every write, never freed. Each settles at the
        /// busiest arrival so far, and an arrival is the frame with the least room to grow one.
        std::vector<VkDescriptorImageInfo> mImageScratch;
        std::vector<VkWriteDescriptorSet> mWriteScratch;
        std::vector<VkBufferImageCopy> mRegionScratch;

        /// Indexed by slot. A slot the scene has freed holds nothing until something takes it over —
        /// `drop` buries the image it had, and the descriptor is left naming what has gone for the
        /// reason `drop` gives.
        std::vector<Texture> mTextures;

        /// One per `TextureWrap`, indexed by it: the sampler a slot is bound through is the one its
        /// file's wrap names, for the texture and for its shading map alike.
        std::array<Sampler, sTextureWrapCount> mSamplers;

        /// One set per frame in flight, both bindings at the maximum the layout declares.
        DescriptorSets mSets;

        /// The last submit that bound each set: a set is bound by handle and carries no stamp of
        /// its own, and a descriptor written under a trace still sampling it is the same hazard as
        /// a table written under one.
        PerSlot<ReadStamp> mBound;

        /// The slots each set has yet to be told, each once however often it was written.
        PerSlot<SlotSet> mOwed;
    };
}

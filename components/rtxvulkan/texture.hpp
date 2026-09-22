#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <osg/Vec4f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/refusal.hpp>
#include <components/rtx/result.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/slots.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/texturewrap.hpp>

#include "descriptorsets.hpp"
#include "frameslots.hpp"
#include "handles.hpp"
#include "image.hpp"
#include "memory.hpp"
#include "readstamp.hpp"
#include "slottable.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class GroundCompositePass;
    class MipChainPass;
    class ShadingPass;
    class SpriteLightPass;

    /// The three dispatches a texture is made with as it arrives, which the renderer owns and
    /// every array is handed together: the chain a file did not carry, the light painted into it,
    /// and a sprite's own light bake.
    struct TexturePasses
    {
        const MipChainPass& mChain;
        const ShadingPass& mShading;
        const SpriteLightPass& mBake;
    };

    /// A sampled image on the GPU, the levels a content file brought for it, and the light the
    /// file already had painted into it — two `Image`s: the first uploaded, the second the shading
    /// map, `SHADING_EXTENT` squared, estimated off the first on the device as it arrives. The map
    /// travels with the texture because it is measured on it and read at its coordinates.
    class Texture
    {
    public:
        /// A slot with nothing in it yet, which is what the array holds while it is being filled.
        Texture() = default;

        /// A texture from its file's bytes, from level `first` on: uploaded level by level where
        /// the file carried a chain, and where it carried one level of more than a texel, uploaded
        /// once and the chain made by `passes.mChain` from that upload — `MipChain` says which
        /// files and why — into a four-byte image of the same curve, the upload buried under the
        /// batch. Why there is none where the device has no room for it as `use`. Every image is
        /// made before anything is recorded, so a refusal leaves the batch as it found it.
        ///
        /// @param passes what makes the chain and estimates the map, or fills the map with the
        ///        neutral one where `data` says the texture is not to be estimated.
        /// @param sampler the sampler the array binds this texture through, which the dispatches
        ///        are handed the texture with.
        /// @param first the level the image begins at: nought for the file as it is, and further
        ///        down for one held to a smaller side. Nought where the device completes the chain,
        ///        which begins at the file's one level.
        /// @param name what a capture calls it. Empty where the build names no objects.
        /// @param regions the caller's scratch, cleared and refilled here with one copy per level.
        static Result<Texture, std::string_view> fromFile(const Device& device, Batch& batch,
            const TexturePasses& passes, VkSampler sampler, const TextureData& data, std::uint32_t first,
            std::string_view name, std::vector<VkBufferImageCopy>& regions, MemoryUse use);

        /// A sprite's light bake: shaped like `source`, made from its alpha by `passes.mBake` in
        /// the same batch, under the neutral map. `source` must stand, and its upload must be
        /// recorded ahead of this, in this batch or in one already submitted.
        static Result<Texture, std::string_view> bakeOf(const Device& device, Batch& batch, const TexturePasses& passes,
            VkSampler sampler, const Texture& source, std::string_view name);

        /// A chunk's flattened ground, stood empty under the neutral map: `GROUND_COMPOSITE_EXTENT`
        /// square, display-encoded, with a chain to one texel and a `UNORM` view a dispatch stores
        /// through. Written by `TextureArray::bakeComposites`, in the placement after it arrives.
        static Result<Texture, std::string_view> composite(const Device& device, Batch& batch, std::string_view name);

        /// One texel of `colour`, whole floats so the value is the one named, under the neutral
        /// map: what `TEXTURE_NEUTRAL` stands, once, when the array is made.
        Texture(const Device& device, Batch& batch, std::string_view name, const osg::Vec4f& colour);
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

        // Read by the tests, the interface's pass, which draws with one, and the array, which
        // hands a composite's image to the bake.
        VkImageView getView() const { return mImage.getView(); }
        const Image& getImage() const { return mImage; }

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
    /// out of one walk, so the two cannot disagree about which slots they counted. A slot that
    /// draws the stand-in holds none.
    struct TexturesHeld
    {
        std::uint32_t mCount = 0;
        VkDeviceSize mBytes = 0;

        /// How many of those stand smaller than their files, from a level further down.
        std::uint32_t mReduced = 0;
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
        /// An array of `slots` textures, none of them written yet: `write` stands them. The length
        /// is the scene's table and not what was described, because a slot the scene has given
        /// back still sits between two that are. A slot nothing describes is one no material
        /// names, which is what `descriptorBindingPartiallyBound` is required for.
        ///
        /// @param layout what `describeLayout` made: every array is shaped by the one the renderer
        ///        keeps, which is what lets one pass be handed any scene's set.
        /// @param passes the renderer's, which every texture is made with as it arrives.
        TextureArray(const Device& device, Batch& batch, const SetLayout& layout, const TexturePasses& passes,
            std::uint32_t slots);

        /// The shape of every set an array here holds: two bindless arrays, partially bound and
        /// updated after bind. Made once by whoever owns the passes that name it.
        static SetLayout describeLayout(const Device& device);

        /// Uploads each of `arrived` into the slot it names, leaving every other texture alone —
        /// why the sets are allocated at the maximum rather than at the scene's count. By slot and
        /// not by appending, because a slot a departing cell freed is taken over wherever it sits.
        /// What a slot held before goes to `graveyard`: a frame in flight may be reading it. The
        /// descriptors are owed to every set and written by `sync`. The bakes go in after every
        /// file, because a bake is made from a source that may be arriving beside it, and the
        /// ground last, for the reason `chooseSide` gives.
        ///
        /// **Every file from the first level within one side**, `chooseSide`'s for the room the
        /// device has left for textures — one side for the whole arrival, so no texture of it is
        /// coarser than another for having come later. A texture the room still cannot hold comes
        /// down a level at a time, and one with no level at all it can hold draws the stand-in, as
        /// does one past the device's side at every level; each of those is appended to `refused`,
        /// saying why.
        void write(Batch& batch, std::span<const TextureData> arrived, std::vector<Refusal>& refused);

        /// Writes the descriptors `slot`'s set owes. Before the placement that binds it, after
        /// `finishReads`: the bindings allow an update after a bind, but not of a descriptor a
        /// pending submit samples, and a trace samples whichever slots its materials name.
        void sync(FrameSlot slot);

        /// Records the bake of every composite that arrived since the last call, into the
        /// composites themselves, reading `slot`'s set and the tables `tables` names — the copy
        /// the placement recording this has just written. After `sync(slot)`, so the set holds the
        /// layers' textures and the composites alike. True where a bake was recorded, because a
        /// placement that recorded nothing else is not submitted.
        bool bakeComposites(VkCommandBuffer commands, const GroundCompositePass& pass, FrameSlot slot,
            const Shaders::GpuTables& tables);

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
        std::uint32_t getCount() const { return static_cast<std::uint32_t>(mSlots.size()); }

        /// Where `slot`'s copy of the texel counts is — `GpuTables::mTextureTexels` — named for
        /// the next submit, which `sync(slot)` brought up to date.
        VkDeviceAddress getTexelsAddress(FrameSlot slot) const { return mTexels.addressFor(slot); }

        /// What the array actually stands. A slot the scene gave back holds nothing and costs
        /// nothing, and neither is counted here.
        TexturesHeld getHeld() const;

        /// The side the last `write` held its files to, which is `getSideLimit` where the room took
        /// nothing off. Read by the tests and by nothing else.
        std::uint32_t getSide() const { return mSide; }

        /// The most texels a side the device takes of every image a texture is made as — the
        /// least `maxExtent` over those images' formats and usages, which is what `vkCreateImage`
        /// is valid against, and never more than `maxImageDimension2D`.
        std::uint32_t getSideLimit() const { return mSideLimit; }

        /// The side `write` holds `arrived` to where the device has `room` bytes for it: the
        /// largest, from the device's own down by halves, at which the arrival comes to no more
        /// than the room. Where it fits at no side, the largest at which its files do without the
        /// ground's composites, which no side brings down and which then take what is left — one
        /// texel where not even that fits, and each texture comes down as far as its file goes.
        std::uint32_t chooseSide(std::span<const TextureData> arrived, VkDeviceSize room) const;

    private:
        /// One slot of the array: the texture standing in it, or the stand-in, which stands once
        /// beside the array for every slot that draws it. One row, so a slot cannot be half
        /// updated.
        struct Slot
        {
            Texture mTexture;
            bool mStandIn = false;

            /// Whether the texture stands smaller than its file.
            bool mReduced = false;

            bool isEmpty() const { return mTexture.isEmpty() && !mStandIn; }
        };

        /// What `slot` is sampled through: its texture, or the stand-in.
        const Texture& standingIn(const Slot& slot) const { return slot.mStandIn ? mStandIn : slot.mTexture; }

        /// What `arrived` would take of the device held to `side`: the bytes uploaded and the
        /// chains and maps the device makes beside them, the ground's composites only where
        /// `ground` says. The resources' own bytes, which the allocator rounds up a little, and the
        /// reason `write` still comes down level by level.
        VkDeviceSize costAt(std::span<const TextureData> arrived, std::uint32_t side, bool ground) const;

        /// What `texture` is made as, held to `side`, or why the device had no room for it.
        Result<Texture, std::string_view> make(
            Batch& batch, const TextureData& texture, std::uint32_t side, std::string_view name);

        /// Stands one of `arrived` in its slot: a texture from its bytes, or a bake from its source,
        /// which must stand already — or the stand-in, where the texture cannot stand.
        void stand(Batch& batch, const TextureData& texture, std::uint32_t side, std::vector<Refusal>& refused);

        /// Queues a write of `image` into `set` at `binding[slot]`, behind the image info the write
        /// names by address.
        static void queueWrite(VkDescriptorSet set, std::uint32_t binding, std::uint32_t slot,
            const VkDescriptorImageInfo& image, std::vector<VkDescriptorImageInfo>& images,
            std::vector<VkWriteDescriptorSet>& writes);

        /// Grows the array to reach `slot`, and refuses one past what the binding holds.
        void reserveSlot(std::uint32_t slot);

        const Device& mDevice;
        const TexturePasses& mPasses;

        /// Cleared and refilled by every describe and every write, never freed. Each settles at the
        /// busiest arrival so far, and an arrival is the frame with the least room to grow one.
        std::vector<VkDescriptorImageInfo> mImageScratch;
        std::vector<VkWriteDescriptorSet> mWriteScratch;
        std::vector<VkBufferImageCopy> mRegionScratch;

        /// Indexed by slot. A slot the scene has freed holds nothing until something takes it over —
        /// `drop` buries the image it had, and the descriptor is left naming what has gone for the
        /// reason `drop` gives.
        std::vector<Slot> mSlots;

        /// One per `TextureWrap`, indexed by it: the sampler a slot is bound through is the one its
        /// file's wrap names, for the texture and for its shading map alike.
        std::array<Sampler, sTextureWrapCount> mSamplers;

        /// The one texel every material with no diffuse names, at `TEXTURE_NEUTRAL` of every set:
        /// beside the array rather than in it, so the array's length stays the scene's table's.
        Texture mNeutral;

        /// `describeStandIn`, stood once, essential: what every slot that cannot stand draws, so a
        /// refusal costs the device nothing and a device with no room left can still refuse.
        Texture mStandIn;

        std::uint32_t mSideLimit = 0;
        std::uint32_t mSide = 0;

        /// The least side the log has said textures are held to, which it says again only for a
        /// smaller one.
        std::uint32_t mSaidSide = 0;

        /// One word per slot of the array, `TEXTURE_SLOTS` long: how many texels the texture in
        /// it holds, which `coneLod` reads where it asked the driver for a size.
        ///
        /// **A copy per frame in flight, owed and paid the way the descriptors are.** A slot the
        /// sweep freed and an arrival took over is one the frame behind still reads through its
        /// material table, so one buffer written as textures stood was a count rewritten under a
        /// trace — a leaf's mask read at the wrong level, and a walk that did not repeat.
        SlotTable<std::uint32_t> mTexels;

        /// One set per frame in flight, both bindings at the maximum the layout declares.
        DescriptorSets mSets;

        /// The last submit that bound each set: a set is bound by handle and carries no stamp of
        /// its own, and a descriptor written under a trace still sampling it is the same hazard as
        /// a table written under one.
        PerSlot<ReadStamp> mBound;

        /// The slots each set has yet to be told, each once however often it was written.
        PerSlot<SlotSet> mOwed;

        /// Which composites arrived and stand empty, and whose ground each is: what the next
        /// placement bakes. Cleared by `bakeComposites` and never freed.
        struct PendingComposite
        {
            Index mSlot;
            Index mMaterial;
        };
        std::vector<PendingComposite> mPendingComposites;
    };
}

#include "texture.hpp"

#include <array>
#include <cassert>
#include <string>
#include <utility>
#include <vector>

#include <components/rtx/contract.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/mipchain.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/shaders/ground.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/shadingmap.h>

#include "commands.hpp"
#include "device.hpp"
#include "graveyard.hpp"
#include "groundcompositepass.hpp"
#include "imageuse.hpp"
#include "mipchainpass.hpp"
#include "shadingpass.hpp"
#include "spritelightpass.hpp"

namespace Rtx
{
    namespace
    {
        /// The map beside a texture, left where the array's sampler expects it: cleared to the
        /// neutral factor, or made by the caller behind this. One level and no chain: the map is
        /// read at level nought whatever the cone, because it has no detail for a level to lose.
        ///
        /// @param neutral whether to clear it, as the float the unorm is rounded from, or to
        ///        leave it undefined for a dispatch to write.
        Image makeShadingMap(const Device& device, Batch& batch, std::string_view name, bool neutral)
        {
            // Built only where something reads it. A release build names no object, and the
            // concatenation is past what a short string holds — so building it anyway is one trip
            // to the heap per texture, for a name that goes nowhere.
            std::string shadingName;
            if constexpr (Device::wantsNames())
                shadingName = std::string(name) + " shading";

            Image map(device, Shaders::SHADING_EXTENT, Shaders::SHADING_EXTENT, VK_FORMAT_R16_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, shadingName);

            if (neutral)
            {
                const VkClearColorValue value{ .float32 = { Shaders::shadingUnit(1.0f), 0.0f, 0.0f, 0.0f } };
                map.clear(batch.getCommands(), Use::sUndefined, value, Use::sTextureSample);
            }

            return map;
        }

        /// What a map costs in the accounting `Texture::getBytes` reports.
        constexpr std::size_t sShadingBytes
            = std::size_t{ Shaders::SHADING_EXTENT } * Shaders::SHADING_EXTENT * sizeof(std::uint16_t);

        /// Four bytes a texel over every level of a loose chain: a third again over the finest.
        std::size_t chainBytes(const Image& chain)
        {
            std::size_t texels = 0;
            for (std::uint32_t level = 0; level < chain.getMipLevels(); ++level)
                texels += std::size_t{ chain.getWidthAt(level) } * chain.getHeightAt(level);
            return texels * 4;
        }

        /// `format` with its transfer curve taken off: the same bytes, read as the bytes they are.
        /// Every format this uploads has such a twin, in the same compatibility class.
        VkFormat withoutCurve(const VkFormat format)
        {
            switch (format)
            {
                case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
                    return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                case VK_FORMAT_BC2_SRGB_BLOCK:
                    return VK_FORMAT_BC2_UNORM_BLOCK;
                case VK_FORMAT_BC3_SRGB_BLOCK:
                    return VK_FORMAT_BC3_UNORM_BLOCK;
                case VK_FORMAT_R8G8B8A8_SRGB:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case VK_FORMAT_B8G8R8A8_SRGB:
                    return VK_FORMAT_B8G8R8A8_UNORM;
                default:
                    return format;
            }
        }

        /// The two arrays the set holds: the textures, and their shading maps at the same slots.
        constexpr std::uint32_t sTextureBinding = 0;
        constexpr std::uint32_t sShadingBinding = 1;

        /// Every stage that resolves a hit, and every dispatch. The trace reads these arrays from
        /// its closest-hit shaders, from the any-hit shader that tests a cutout and from the miss
        /// shader that draws the sky; the fog volume, the tone curve and the interface are
        /// dispatches and read them too.
        constexpr VkShaderStageFlags sStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR
            | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;

        /// The layout every array declares, sized to the maximum and not to the scene, because a
        /// pipeline outlives a cell and two set layouts are compatible only where they are
        /// identically defined. The maximum costs a few hundred kilobytes of pool, paid once.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sBindings{
            VkDescriptorSetLayoutBinding{
                sTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Shaders::TEXTURE_SLOTS, sStages },
            VkDescriptorSetLayoutBinding{
                sShadingBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, Shaders::TEXTURE_SLOTS, sStages },
        };
    }

    VkFormat toVulkanFormat(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::Bc1RgbaSrgb:
                return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case TextureFormat::Bc2Srgb:
                return VK_FORMAT_BC2_SRGB_BLOCK;
            case TextureFormat::Bc3Srgb:
                return VK_FORMAT_BC3_SRGB_BLOCK;
            case TextureFormat::Rgba8Unorm:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case TextureFormat::Rgba8Srgb:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case TextureFormat::Bgra8Srgb:
                return VK_FORMAT_B8G8R8A8_SRGB;

            // Never uploaded: `describeImage` refuses them, so one arriving here is a contract
            // broken and not a file.
            case TextureFormat::Rgb8:
            case TextureFormat::Luminance:
            case TextureFormat::LuminanceAlpha:
            case TextureFormat::Unnamed:
                break;
        }

        // A format nothing above named: a new one that forgets a case lands here rather than
        // creating an image with a format nobody chose.
        throw Error("a texture format this renderer does not upload");
    }

    Texture::Texture(const Device& device, Batch& batch, const TexturePasses& passes, const VkSampler sampler,
        const TextureData& data, std::string_view name, std::vector<VkBufferImageCopy>& regions)
    {
        assert(!data.mLevels.empty());

        const auto levels = static_cast<std::uint32_t>(data.mLevels.size());

        // Every level in one submit: the levels are already contiguous in the source, so this is one
        // copy per level out of one buffer rather than one upload per level.
        regions.clear();
        regions.reserve(levels);
        for (std::uint32_t level = 0; level < levels; ++level)
            regions.push_back(VkBufferImageCopy{
                .bufferOffset = data.mLevels[level].mOffset,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                .imageExtent = { data.mLevels[level].mWidth, data.mLevels[level].mHeight, 1 },
            });

        if (!data.mCompleteChain)
        {
            mImage = Image(device, data.mWidth, data.mHeight, toVulkanFormat(data.mFormat),
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, name, levels);
            uploadImage(batch, mImage, data.mBytes, regions);
            mBytes = data.mBytes.size();
        }
        else
        {
            assert(MipChain::wantedFor(data) && "a chain completed for a file that has one");

            // The file's one level, uploaded as the bytes it holds, in a format with no curve under
            // it so that the chain's first dispatch fetches those bytes and not the light behind
            // them; gone with the batch, because the chain is what the trace samples.
            Image upload(device, data.mWidth, data.mHeight, withoutCurve(toVulkanFormat(data.mFormat)),
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, name, 1);
            uploadImage(batch, upload, data.mBytes, regions);

            // Four bytes a texel down to one texel, with the file's own curve over the sampler's
            // read and none over the dispatch's store — `MipChain` says why the chain is loose.
            const bool encoded = isSrgb(data.mFormat);
            mImage = Image(device, data.mWidth, data.mHeight,
                encoded ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, name, levelsTo1x1(data.mWidth, data.mHeight),
                1, encoded ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED);
            passes.mChain.record(batch.getCommands(), upload, sampler, mImage, encoded);
            batch.keep(std::move(upload));

            mBytes = chainBytes(mImage);
        }

        mWrap = data.mWrap;

        // Estimated off the texture just made, by a dispatch behind it, or cleared to the neutral
        // factor where nothing is to be estimated — `TextureData::hasNeutralShading`.
        const bool neutral = data.hasNeutralShading();
        mShading = makeShadingMap(device, batch, name, neutral);
        if (!neutral)
            passes.mShading.record(batch.getCommands(), mImage, sampler, mShading, data);

        mBytes += sShadingBytes;
    }

    Texture::Texture(const Device& device, Batch& batch, const TexturePasses& passes, const VkSampler sampler,
        const Texture& source, std::string_view name)
    {
        assert(!source.isEmpty());

        const Image& from = source.mImage;
        mImage = Image(device, from.getWidth(), from.getHeight(), VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, name, from.getMipLevels());
        passes.mBake.record(batch.getCommands(), from, sampler, mImage);

        // Clamped, because a bake is one image whose coordinates run edge to edge — what
        // `TextureTable::addBaked` says of its row.
        mWrap = TextureWrap::Clamp;

        // Neutral, because nothing divides a bake by a map, and the array binds one at every slot.
        mShading = makeShadingMap(device, batch, name, true);

        mBytes = chainBytes(mImage) + sShadingBytes;
    }

    Texture::Texture(const Device& device, Batch& batch, const std::string_view name, const osg::Vec4f& colour)
    {
        mImage = Image(device, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, name, 1);

        const std::array<float, 4> texel{ colour.x(), colour.y(), colour.z(), colour.w() };
        std::array<VkBufferImageCopy, 1> regions{ VkBufferImageCopy{
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { 1, 1, 1 },
        } };
        uploadImage(batch, mImage, std::as_bytes(std::span<const float>(texel)), regions);

        mShading = makeShadingMap(device, batch, name, true);
        mBytes = sizeof(texel) + sShadingBytes;
    }

    Texture::Texture(const Device& device, Batch& batch, std::string_view name)
    {
        // A chain to one texel, which the bake blits down from the level it writes; both transfer
        // usages for that blit, and the `UNORM` view for the store.
        constexpr std::uint32_t extent = Shaders::GROUND_COMPOSITE_EXTENT;
        mImage = Image(device, extent, extent, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            name, levelsTo1x1(extent, extent), 1, VK_FORMAT_R8G8B8A8_UNORM);

        // Clamped, because a composite is one image whose coordinates run edge to edge — what
        // `TextureTable::addBaked` says of its row.
        mWrap = TextureWrap::Clamp;

        // Neutral, because the light painted into each ground texture came off per tile in the
        // bake, which is the only place the tiling is known; an estimate off the composite would
        // take it off twice.
        mShading = makeShadingMap(device, batch, name, true);

        mBytes = chainBytes(mImage) + sShadingBytes;
    }

    SetLayout TextureArray::describeLayout(const Device& device)
    {
        // Partially bound because a scene with fewer textures than the array can hold leaves the
        // tail unwritten. Update after bind, because an arrival writes this set while work that
        // named it is still on the queue — legal as long as no pending command reads that
        // descriptor, and a slot nothing has described is a slot no material names.
        constexpr VkDescriptorBindingFlags sBound
            = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        constexpr std::array<VkDescriptorBindingFlags, 2> flags{ sBound, sBound };
        const VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = static_cast<std::uint32_t>(flags.size()),
            .pBindingFlags = flags.data(),
        };

        return makeSetLayout(
            device, sBindings, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT_EXT, &bindingFlags);
    }

    TextureArray::TextureArray(const Device& device, Batch& batch, const SetLayout& layout, const TexturePasses& passes,
        const std::uint32_t slots, std::span<const TextureData> textures)
        : mDevice(device)
        , mPasses(passes)
        , mSamplers{ makeContentSampler(device, "textures repeating", TextureWrap::Repeat),
            makeContentSampler(device, "textures clamped along s", TextureWrap::ClampS),
            makeContentSampler(device, "textures clamped along t", TextureWrap::ClampT),
            makeContentSampler(device, "textures clamped", TextureWrap::Clamp) }
        // Allocated at the maximum the layout declares, not at what this scene brought. Sizing the
        // set to the cell is what made a texture arriving mean a new set, a new pool and every
        // image uploaded again; four thousand descriptors is a few hundred kilobytes of pool and it
        // is paid once. `write` then only ever owes the slots that are new.
        , mNeutral(device, batch, "neutral texel",
              osg::Vec4f(
                  Shaders::NO_TEXTURE_ALBEDO.x(), Shaders::NO_TEXTURE_ALBEDO.y(), Shaders::NO_TEXTURE_ALBEDO.z(), 1.0f))
        , mSets(device, sBindings, layout.get(), sFrameSlots, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT)
    {
        // The last slot is the neutral texel's and the scene may not reach it.
        if (slots > Shaders::TEXTURE_NEUTRAL)
            throw Error("a scene with " + std::to_string(slots) + " textures is past the "
                + std::to_string(Shaders::TEXTURE_NEUTRAL) + " this array holds beside its neutral texel");

        // The neutral texel's count, and nought for every slot nothing stands, which no material
        // names. Owed to every copy and every set from the start, the way an arrival is: written by
        // the `sync` before the first placement that binds them.
        mTexels.open(device, sFrameSlots, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "texture texels");
        mTexels.resize(Shaders::TEXTURE_SLOTS);
        mTexels.write(Shaders::TEXTURE_NEUTRAL) = 1;
        for (SlotSet& owed : mOwed.live())
            owed.addMakingRoom(Shaders::TEXTURE_NEUTRAL);

        // Sized to the table before anything is written into it, so a description lands in the
        // slot it names whatever sits either side of it. Every entry starts holding no image and no
        // map, which is what a free slot goes on holding: its descriptors are never written, and
        // the bindings' `PARTIALLY_BOUND` is what makes that legal for one nothing samples.
        mTextures.resize(slots);

        write(batch, textures);
    }

    void TextureArray::reserveSlot(std::uint32_t slot)
    {
        if (slot >= Shaders::TEXTURE_NEUTRAL)
            throw Error("a scene wanting texture slot " + std::to_string(slot) + " is past the "
                + std::to_string(Shaders::TEXTURE_NEUTRAL) + " this array holds beside its neutral texel");

        // Grown to reach it rather than one at a time: arrivals come in whatever order the scene's
        // free list handed the slots out, so the highest is not always the last.
        if (slot >= mTextures.size())
            mTextures.resize(slot + 1);
    }

    void TextureArray::write(Batch& batch, std::span<const TextureData> arrived)
    {
        if (arrived.empty())
            return;

        // Every source before every bake, because a bake is made from the texture standing in the
        // slot it names. Two walks and not a sort: the order is the kind's, and `arrived` is what
        // one cell brought.
        for (const TextureData& texture : arrived)
            if (texture.mSource != TextureSource::SpriteBake)
                stand(batch, texture);

        for (const TextureData& texture : arrived)
            if (texture.mSource == TextureSource::SpriteBake)
                stand(batch, texture);
    }

    void TextureArray::stand(Batch& batch, const TextureData& texture)
    {
        reserveSlot(texture.mSlot);

        // Named only where a capture or a validation message could read it back. A local,
        // because a slot number is short enough that this never reaches the heap; the one that
        // does is the map's, inside `Texture`.
        std::string name;
        if constexpr (Device::wantsNames())
            name = "texture " + std::to_string(texture.mSlot);

        const VkSampler sampler = mSamplers[static_cast<std::size_t>(texture.mWrap)].get();

        // What the slot held is buried and not destroyed: its descriptor is the one a frame in
        // flight bound, and it stays valid until the timeline says nothing reads it.
        switch (texture.mSource)
        {
            case TextureSource::GroundComposite:
                mDevice.getGraveyard().replace(mTextures[texture.mSlot], Texture(mDevice, batch, name));
                mPendingComposites.push_back(PendingComposite{ .mSlot = texture.mSlot, .mMaterial = texture.mFrom });
                break;

            case TextureSource::SpriteBake:
                // The source stands: `SceneTextures` names one only where the table holds it live,
                // a live slot is described whenever it arrives, and `write` stands every source
                // ahead of every bake. A bake of a slot that holds nothing is a contract broken and
                // not content.
                contract(texture.mFrom < mTextures.size() && !mTextures[texture.mFrom].isEmpty(),
                    "a sprite light bake names a source that does not stand");
                mDevice.getGraveyard().replace(mTextures[texture.mSlot],
                    Texture(mDevice, batch, mPasses, sampler, mTextures[texture.mFrom], name));
                break;

            case TextureSource::File:
            case TextureSource::StandIn:
                mDevice.getGraveyard().replace(
                    mTextures[texture.mSlot], Texture(mDevice, batch, mPasses, sampler, texture, name, mRegionScratch));
                break;
        }

        // How many texels the slot now holds, for `coneLod`, owed to every copy beside the
        // descriptor owed to every set.
        const Image& stood = mTextures[texture.mSlot].getImage();
        mTexels.write(texture.mSlot) = stood.getWidth() * stood.getHeight();

        for (SlotSet& owed : mOwed.live())
            owed.addMakingRoom(texture.mSlot);
    }

    VkDescriptorSet TextureArray::getSet(const FrameSlot slot) const
    {
        assert(slot.get() < sFrameSlots);
        mBound.at(slot).nameFor(mDevice.getTimeline().getNext());
        return mSets.get(slot.get());
    }

    void TextureArray::finishReads(const FrameSlot slot) const
    {
        mBound.at(slot).waitIdle(mDevice, "a trace still sampling a texture set");
        mTexels.finishReads(slot);
    }

    void TextureArray::sync(const FrameSlot slot)
    {
        mTexels.sync(slot);

        SlotSet& owed = mOwed.at(slot);
        if (owed.empty())
            return;

        assert(mBound.at(slot).isIdle(mDevice) && "a descriptor written under a submit still bound to it");

        // One write per slot and per array rather than one over a range: the arrivals are wherever
        // the scene's free list put them, and a run is no longer what they are. Reserved before
        // any write points into it, since a write names its image by address.
        const std::span<const Index> slots = owed.getSlots();
        mImageScratch.clear();
        mWriteScratch.clear();
        mImageScratch.reserve(2 * slots.size());
        mWriteScratch.reserve(2 * slots.size());

        const VkDescriptorSet set = mSets.get(slot.get());
        for (const Index at : slots)
        {
            // Owed and since dropped: the slot holds nothing, and a descriptor left naming what has
            // gone is what `drop` says is legal. The neutral texel stands beside the array.
            const Texture& held = at == Shaders::TEXTURE_NEUTRAL ? mNeutral : mTextures[at];
            if (held.isEmpty())
                continue;

            const VkSampler sampler = mSamplers[static_cast<std::size_t>(held.getWrap())].get();
            queueWrite(set, sTextureBinding, at, held.describe(sampler), mImageScratch, mWriteScratch);
            queueWrite(set, sShadingBinding, at, held.describeShading(sampler), mImageScratch, mWriteScratch);
        }

        updateSets(mDevice, mWriteScratch);

        owed.clear();
    }

    bool TextureArray::bakeComposites(const VkCommandBuffer commands, const GroundCompositePass& pass,
        const FrameSlot slot, const Shaders::GpuTables& tables)
    {
        if (mPendingComposites.empty())
            return false;

        bool baked = false;
        const VkDescriptorSet set = getSet(slot);
        for (const PendingComposite& pending : mPendingComposites)
        {
            // Arrived and since dropped, before any placement baked it: the slot holds nothing,
            // and a bake of nothing is nothing to record.
            const Texture& held = mTextures[pending.mSlot];
            if (held.isEmpty())
                continue;

            pass.record(commands, set, held.getImage(),
                Shaders::GroundCompositeConstants{
                    .mMaterials = tables.mMaterials,
                    .mLayers = tables.mLayers,
                    .mMasks = tables.mMasks,
                    .mMaterial = pending.mMaterial,
                });
            baked = true;
        }

        mPendingComposites.clear();
        return baked;
    }

    void TextureArray::drop(std::span<const std::uint32_t> slots)
    {
        for (const std::uint32_t slot : slots)
        {
            // A slot this array never held: a scene can add a texture and sweep it in the same
            // window, before anything was handed over to upload it.
            if (slot >= mTextures.size())
                continue;

            // Exchanged rather than erased, so the slot stays where it is and the image goes under
            // the frame that may still name it.
            mDevice.getGraveyard().replace(mTextures[slot], Texture());
        }
    }

    void TextureArray::queueWrite(const VkDescriptorSet set, const std::uint32_t binding, const std::uint32_t slot,
        const VkDescriptorImageInfo& image, std::vector<VkDescriptorImageInfo>& images,
        std::vector<VkWriteDescriptorSet>& writes)
    {
        images.push_back(image);
        writes.push_back(VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = binding,
            .dstArrayElement = slot,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &images.back(),
        });
    }

    TexturesHeld TextureArray::getHeld() const
    {
        TexturesHeld held;

        for (const Texture& texture : mTextures)
        {
            // Whether it is there and not its size: a slot stands a texture or it does not, and a
            // content file carrying an empty level is a texture that exists.
            if (texture.isEmpty())
                continue;

            ++held.mCount;
            held.mBytes += texture.getBytes();
        }

        return held;
    }
}

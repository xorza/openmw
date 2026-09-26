#include "texture.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <components/crashcatcher/crashnote.hpp>
#include <components/debug/debuglog.hpp>
#include <components/rtx/contract.hpp>
#include <components/rtx/mipchain.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/shaders/ground.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/shadingmap.h>
#include <components/rtx/texturebuilder.hpp>

#include "commands.hpp"
#include "device.hpp"
#include "formats.hpp"
#include "graveyard.hpp"
#include "groundcompositepass.hpp"
#include "imageuse.hpp"
#include "mipchainpass.hpp"
#include "physicaldevice.hpp"
#include "result.hpp"
#include "shadingpass.hpp"
#include "spritelightpass.hpp"

namespace Rtx
{
    namespace
    {
        /// The map beside a texture: one level and no chain, because the map is read at level
        /// nought whatever the cone and has no detail for a level to lose. Left undefined, for a
        /// dispatch to write or `clearNeutral` to fill.
        Result<Image, std::string_view> makeShadingMap(const Device& device, std::string_view name, MemoryUse use)
        {
            // Built only where something reads it. A release build names no object, and the
            // concatenation is past what a short string holds — so building it anyway is one trip
            // to the heap per texture, for a name that goes nowhere.
            std::string shadingName;
            if constexpr (Device::wantsNames())
                shadingName = std::string(name) + " shading";

            return Image::tryMake(use, device, Shaders::SHADING_EXTENT, Shaders::SHADING_EXTENT,
                toVulkanFormat(SHADING_MAP_FORMAT),
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, shadingName);
        }

        /// Fills `map` with the neutral factor, as the float the unorm is rounded from, and leaves
        /// it where the array's sampler expects it.
        void clearNeutral(Batch& batch, const Image& map)
        {
            const VkClearColorValue value{ .float32 = { Shaders::shadingUnit(1.0f), 0.0f, 0.0f, 0.0f } };
            map.clear(batch.getCommands(), Use::sUndefined, value, Use::sTextureSample);
        }

        /// What a map costs in the accounting `Texture::getBytes` reports.
        constexpr std::size_t sShadingBytes
            = std::size_t{ Shaders::SHADING_EXTENT } * Shaders::SHADING_EXTENT * sizeof(std::uint16_t);

        /// Four bytes a texel over `levels` levels of a loose chain from `width` by `height` down —
        /// a third again over the finest, where it goes to one texel.
        std::size_t chainBytes(std::uint32_t width, std::uint32_t height, std::uint32_t levels)
        {
            std::size_t texels = 0;
            for (std::uint32_t level = 0; level < levels; ++level)
                texels += std::size_t{ std::max(width >> level, 1u) } * std::max(height >> level, 1u);
            return texels * 4;
        }

        /// The same over the levels `chain` holds.
        std::size_t chainBytes(const Image& chain)
        {
            return chainBytes(chain.getWidth(), chain.getHeight(), chain.getMipLevels());
        }

        /// A format with a transfer curve and its twin without one: the same bytes in the same
        /// compatibility class, read through the curve or as the bytes they are.
        struct CurveTwins
        {
            VkFormat mEncoded;
            VkFormat mLinear;
        };

        /// Every format this uploads or writes that has a curve, beside its twin — one table, so
        /// the two directions below cannot disagree.
        constexpr std::array sCurveTwins{
            CurveTwins{ VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK },
            CurveTwins{ VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK },
            CurveTwins{ VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK },
            CurveTwins{ VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM },
            CurveTwins{ VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM },
        };

        /// `format` with its transfer curve taken off: the same bytes, read as the bytes they are.
        /// A format with no curve is its own.
        constexpr VkFormat withoutCurve(const VkFormat format)
        {
            for (const CurveTwins& twins : sCurveTwins)
                if (twins.mEncoded == format)
                    return twins.mLinear;
            return format;
        }

        /// `format` read through a transfer curve, which is how the trace samples a written texture
        /// whose file was display-encoded.
        constexpr VkFormat withCurve(const VkFormat format)
        {
            for (const CurveTwins& twins : sCurveTwins)
                if (twins.mLinear == format)
                    return twins.mEncoded;
            broken("a format with no twin under a curve");
        }

        /// What a written texture is stored as, which is what the shaders that write it declare,
        /// and the same bytes through the curve.
        constexpr VkFormat sWrittenFormat = toVulkanFormat(TEXTURE_WRITTEN_FORMAT);
        constexpr VkFormat sWrittenEncoded = withCurve(sWrittenFormat);

        /// What a texture made on the device is created as: the format its description states.
        ///
        /// **The description's, because whether the trace reads it through the curve is a fact
        /// about the content** — a bake is lighting and a composite is albedo — and the core is
        /// what states it. A dispatch stores through `sWrittenFormat`, the same bytes with the curve
        /// off, so a description may name that or its twin under the curve and nothing else.
        VkFormat writtenAs(const TextureFormat format)
        {
            const VkFormat image = toVulkanFormat(format);
            contract(
                withoutCurve(image) == sWrittenFormat, "a texture described in a format its dispatch does not store");
            return image;
        }

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
            VkDescriptorSetLayoutBinding{ Shaders::TEXTURE_BIND_IMAGES, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                Shaders::TEXTURE_SLOTS, sStages },
            VkDescriptorSetLayoutBinding{ Shaders::TEXTURE_BIND_SHADING, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                Shaders::TEXTURE_SLOTS, sStages },
        };

        constexpr VkImageUsageFlags sUploaded = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        constexpr VkImageUsageFlags sWritten = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        /// `TextureArray::getSideLimit`, asked of the device once, over every image a file or a
        /// bake is made as: each format uploaded, its twin without the curve a chain is completed
        /// from, and the four-byte chain and bake. A composite is left out, because its side is the
        /// renderer's own.
        std::uint32_t sideLimitOf(const Device& device)
        {
            const PhysicalDevice& physical = device.getPhysicalDevice();
            std::uint32_t side = physical.getProperties().mProperties2.properties.limits.maxImageDimension2D;

            const auto takes
                = [&](const VkFormat format, const VkImageUsageFlags usage, const VkImageCreateFlags flags) {
                      VkImageFormatProperties properties{};
                      checkVk(vkGetPhysicalDeviceImageFormatProperties(physical.getHandle(), format, VK_IMAGE_TYPE_2D,
                                  VK_IMAGE_TILING_OPTIMAL, usage, flags, &properties),
                          "vkGetPhysicalDeviceImageFormatProperties");
                      side = std::min({ side, properties.maxExtent.width, properties.maxExtent.height });
                  };

            for (std::size_t at = 0; at < sTextureFormatCount; ++at)
            {
                const auto format = static_cast<TextureFormat>(at);
                if (!isUploadable(format))
                    continue;

                takes(toVulkanFormat(format), sUploaded, 0);
                takes(withoutCurve(toVulkanFormat(format)), sUploaded, 0);
            }

            takes(sWrittenEncoded, sWritten, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
            takes(sWrittenFormat, sWritten, 0);

            return side;
        }

        /// Where `texture` begins held to `side`: its first level within it, or its last where the
        /// file carries none that small, which is as far down as it goes.
        std::uint32_t firstLevelAt(const TextureData& texture, std::uint32_t side)
        {
            return texture.firstLevelWithin(side).value_or(static_cast<std::uint32_t>(texture.mLevels.size()) - 1);
        }

        /// The next side a search tries below `side`: the power of two under it.
        std::uint32_t sideBelow(std::uint32_t side)
        {
            assert(side > 1);
            return std::bit_floor(side - 1);
        }

        /// Why a texture draws the stand-in where the device takes none of its levels.
        std::string pastTheSide(const TextureData& texture, std::uint32_t limit)
        {
            const MipLevel& last = texture.mLevels.back();
            return "its smallest level is " + std::to_string(last.mWidth) + " by " + std::to_string(last.mHeight)
                + " texels, and the device takes " + std::to_string(limit) + " a side";
        }
    }

    Result<Texture, std::string_view> Texture::fromFile(const Device& device, Batch& batch, const TexturePasses& passes,
        const VkSampler sampler, const TextureData& data, const std::uint32_t first, std::string_view name,
        std::vector<VkBufferImageCopy>& regions, const MemoryUse use)
    {
        assert(first < data.mLevels.size() && "a texture begun past the file's last level");
        assert((first == 0 || !data.mCompleteChain) && "a chain completed from a level the file did not begin at");

        Crash::note("staging the texture", data.mName);

        const MipLevel& top = data.mLevels[first];
        const auto levels = static_cast<std::uint32_t>(data.mLevels.size()) - first;
        const std::span<const std::byte> bytes = data.mBytes.subspan(top.mOffset);

        // Every level in one submit: the levels are already contiguous in the source, so this is one
        // copy per level out of one buffer rather than one upload per level — and from the first
        // level on, so a texture held to a smaller side stages none of what it leaves out.
        regions.clear();
        regions.reserve(levels);
        for (std::uint32_t level = 0; level < levels; ++level)
        {
            const MipLevel& from = data.mLevels[first + level];
            regions.push_back(VkBufferImageCopy{
                .bufferOffset = from.mOffset - top.mOffset,
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                .imageExtent = { from.mWidth, from.mHeight, 1 },
            });
        }

        Texture made;
        made.mWrap = data.mWrap;

        // Estimated off the texture about to be made, by a dispatch behind it, or cleared to the
        // neutral factor where nothing is to be estimated — `TextureData::hasNeutralShading`.
        Result<Image, std::string_view> shading = makeShadingMap(device, name, use);
        if (!shading.isOk())
            return Err{ shading.error() };

        if (!data.mCompleteChain)
        {
            Result<Image, std::string_view> image = Image::tryMake(
                use, device, top.mWidth, top.mHeight, toVulkanFormat(data.mFormat), sUploaded, name, levels);
            if (!image.isOk())
                return Err{ image.error() };

            made.mImage = std::move(image.value());
            uploadImage(batch, made.mImage, bytes, regions);
            made.mBytes = bytes.size();
        }
        else
        {
            assert(MipChain::wantedFor(data) && "a chain completed for a file that has one");

            // The file's one level, uploaded as the bytes it holds, in a format with no curve under
            // it so that the chain's first dispatch fetches those bytes and not the light behind
            // them; gone with the batch, because the chain is what the trace samples.
            Result<Image, std::string_view> upload = Image::tryMake(
                use, device, data.mWidth, data.mHeight, withoutCurve(toVulkanFormat(data.mFormat)), sUploaded, name, 1);
            if (!upload.isOk())
                return Err{ upload.error() };

            // Four bytes a texel down to one texel, with the file's own curve over the sampler's
            // read and none over the dispatch's store — `MipChain` says why the chain is loose.
            const bool encoded = isSrgb(data.mFormat);
            Result<Image, std::string_view> chain = Image::tryMake(use, device, data.mWidth, data.mHeight,
                encoded ? sWrittenEncoded : sWrittenFormat, sWritten, name, levelsTo1x1(data.mWidth, data.mHeight), 1,
                encoded ? sWrittenFormat : VK_FORMAT_UNDEFINED);
            if (!chain.isOk())
                return Err{ chain.error() };

            uploadImage(batch, upload.value(), data.mBytes, regions);
            made.mImage = std::move(chain.value());
            passes.mChain.record(batch.getCommands(), upload.value(), sampler, made.mImage, encoded);
            batch.keep(std::move(upload.value()));

            made.mBytes = chainBytes(made.mImage);
        }

        made.mShading = std::move(shading.value());
        if (data.hasNeutralShading())
            clearNeutral(batch, made.mShading);
        else
            passes.mShading.record(batch.getCommands(), made.mImage, sampler, made.mShading, data);

        made.mBytes += sShadingBytes;
        return made;
    }

    Result<Texture, std::string_view> Texture::bakeOf(const Device& device, Batch& batch, const TexturePasses& passes,
        const VkSampler sampler, const Texture& source, const TextureFormat format, std::string_view name)
    {
        assert(!source.isEmpty());

        const Image& from = source.mImage;
        Result<Image, std::string_view> image = Image::tryMake(MemoryUse::Texture, device, from.getWidth(),
            from.getHeight(), writtenAs(format), sWritten, name, from.getMipLevels(), 1, sWrittenFormat);
        if (!image.isOk())
            return Err{ image.error() };

        // Neutral, because nothing divides a bake by a map, and the array binds one at every slot.
        Result<Image, std::string_view> shading = makeShadingMap(device, name, MemoryUse::Texture);
        if (!shading.isOk())
            return Err{ shading.error() };

        Texture made;
        made.mImage = std::move(image.value());
        passes.mBake.record(batch.getCommands(), from, sampler, made.mImage);

        // Clamped, because a bake is one image whose coordinates run edge to edge — what
        // `TextureTable::addBaked` says of its row.
        made.mWrap = TextureWrap::Clamp;

        made.mShading = std::move(shading.value());
        clearNeutral(batch, made.mShading);

        made.mBytes = chainBytes(made.mImage) + sShadingBytes;
        return made;
    }

    Texture::Texture(const Device& device, Batch& batch, const std::string_view name, const osg::Vec4f& colour)
    {
        mImage = Image(device, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sUploaded, name, 1);

        const std::array<float, 4> texel{ colour.x(), colour.y(), colour.z(), colour.w() };
        std::array<VkBufferImageCopy, 1> regions{ VkBufferImageCopy{
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { 1, 1, 1 },
        } };
        uploadImage(batch, mImage, std::as_bytes(std::span<const float>(texel)), regions);

        mShading = std::move(makeShadingMap(device, name, MemoryUse::Essential).value());
        clearNeutral(batch, mShading);
        mBytes = sizeof(texel) + sShadingBytes;
    }

    Result<Texture, std::string_view> Texture::composite(
        const Device& device, Batch& batch, const TextureFormat format, std::string_view name)
    {
        // A chain to one texel, which the bake blits down from the level it writes; both transfer
        // usages for that blit, and the view without the curve for the store.
        constexpr std::uint32_t extent = Shaders::GROUND_COMPOSITE_EXTENT;
        Result<Image, std::string_view> image = Image::tryMake(MemoryUse::Texture, device, extent, extent,
            writtenAs(format), sWritten | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, name,
            levelsTo1x1(extent, extent), 1, sWrittenFormat);
        if (!image.isOk())
            return Err{ image.error() };

        // Neutral, because the light painted into each ground texture came off per tile in the
        // bake, which is the only place the tiling is known; an estimate off the composite would
        // take it off twice.
        Result<Image, std::string_view> shading = makeShadingMap(device, name, MemoryUse::Texture);
        if (!shading.isOk())
            return Err{ shading.error() };

        Texture made;
        made.mImage = std::move(image.value());

        // Clamped, because a composite is one image whose coordinates run edge to edge — what
        // `TextureTable::addBaked` says of its row.
        made.mWrap = TextureWrap::Clamp;

        made.mShading = std::move(shading.value());
        clearNeutral(batch, made.mShading);

        made.mBytes = chainBytes(made.mImage) + sShadingBytes;
        return made;
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
        const std::uint32_t slots)
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
        , mSideLimit(sideLimitOf(device))
        , mSide(mSideLimit)
        , mSaidSide(mSideLimit)
        , mSets(device, sBindings, layout.get(), sFrameSlots, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT)
    {
        // Essential, and the one texture made that way: it is what stands where the device had no
        // room for the texture itself.
        mStandIn = std::move(Texture::fromFile(device, batch, passes, mSamplers[0].get(), describeStandIn(), 0,
            "stand-in", mRegionScratch, MemoryUse::Essential)
                                 .value());

        // The last slot is the neutral texel's. `TextureTable` refuses a slot past it, so a scene
        // that reaches it is a table that broke that rule.
        contract(slots <= Shaders::TEXTURE_NEUTRAL, "a scene with more textures than its table may hand out");

        // The neutral texel's count, and nought for every slot nothing stands, which no material
        // names. Owed to every copy and every set from the start, the way an arrival is: written by
        // the `sync` before the first placement that binds them.
        mTexels.open(device, sFrameSlots, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "texture texels");
        mTexels.grow(Shaders::TEXTURE_SLOTS);
        mTexels.write(Shaders::TEXTURE_NEUTRAL) = 1;
        for (SlotSet& owed : mOwed.live())
            owed.addMakingRoom(Shaders::TEXTURE_NEUTRAL);

        // Sized to the table before anything is written into it, so a description lands in the
        // slot it names whatever sits either side of it. Every entry starts holding no image and no
        // map, which is what a free slot goes on holding: its descriptors are never written, and
        // the bindings' `PARTIALLY_BOUND` is what makes that legal for one nothing samples.
        mSlots.resize(slots);
    }

    void TextureArray::reserveSlot(std::uint32_t slot)
    {
        contract(slot < Shaders::TEXTURE_NEUTRAL, "a texture slot past what its table may hand out");

        // Grown to reach it rather than one at a time: arrivals come in whatever order the scene's
        // free list handed the slots out, so the highest is not always the last.
        if (slot >= mSlots.size())
            mSlots.resize(slot + 1);
    }

    void TextureArray::write(Batch& batch, std::span<const TextureData> arrived, std::vector<Refusal>& refused)
    {
        if (arrived.empty())
            return;

        const std::uint32_t side = chooseSide(arrived, mDevice.getMemory().getRoom(MemoryUse::Texture));

        // Said for a side smaller than any said before, and not per arrival: a device short of room
        // holds every arrival to about the same side, and the log would say it at every crossing.
        if (side < mSaidSide)
        {
            Log(Debug::Warning) << "Ray tracing holds the textures that arrive to " << side
                                << " texels a side, which is what the device has room for";
            mSaidSide = side;
        }
        mSide = side;

        // Every source before every bake, because a bake is made from the texture standing in the
        // slot it names, and the ground last, because it is what gives way where the room runs out
        // — `chooseSide` says why. Three walks and not a sort: the order is the kind's, and
        // `arrived` is what one cell brought.
        for (const TextureData& texture : arrived)
            if (texture.mSource == TextureSource::File || texture.mSource == TextureSource::StandIn)
                stand(batch, texture, side, refused);

        for (const TextureData& texture : arrived)
            if (texture.mSource == TextureSource::SpriteBake)
                stand(batch, texture, side, refused);

        for (const TextureData& texture : arrived)
            if (texture.mSource == TextureSource::GroundComposite || texture.mSource == TextureSource::GroundGloss)
                stand(batch, texture, side, refused);
    }

    std::uint32_t TextureArray::chooseSide(std::span<const TextureData> arrived, const VkDeviceSize room) const
    {
        const auto largestAt = [&](const bool ground) {
            std::uint32_t side = mSideLimit;
            while (side > 1 && costAt(arrived, side, ground) > room)
                side = sideBelow(side);

            return side;
        };

        // **The whole arrival, where it fits at some side.** Where it fits at none, the ground's
        // composites, which no side brings down, are more than the room by themselves, and holding
        // the files to one texel would buy them nothing: the files are held to the side they fit
        // at alone, and the ground, stood last, takes what they leave. What is nearest the eye
        // keeps its detail, and what gives way is the distance.
        const std::uint32_t whole = largestAt(true);
        if (costAt(arrived, whole, true) <= room)
            return whole;

        return largestAt(false);
    }

    VkDeviceSize TextureArray::costAt(
        std::span<const TextureData> arrived, const std::uint32_t side, const bool ground) const
    {
        // A bake is shaped like its source as the source is held to `side`: from its first level
        // within the side where it arrives beside the bake, as the stand-in where it arrives as one,
        // and as its image where it stands already.
        const auto bakeOf = [&](const Index slot) -> VkDeviceSize {
            const auto arriving = std::find_if(
                arrived.begin(), arrived.end(), [&](const TextureData& texture) { return texture.mSlot == slot; });

            const Image* standing = nullptr;
            if (arriving != arrived.end())
            {
                if (arriving->mSource == TextureSource::File && arriving->firstLevelWithin(mSideLimit).has_value())
                {
                    const std::uint32_t first = firstLevelAt(*arriving, side);
                    const MipLevel& top = arriving->mLevels[first];
                    return chainBytes(
                               top.mWidth, top.mHeight, static_cast<std::uint32_t>(arriving->mLevels.size()) - first)
                        + sShadingBytes;
                }

                standing = &mStandIn.getImage();
            }
            else if (slot < mSlots.size())
                standing = &standingIn(mSlots[slot]).getImage();

            if (standing == nullptr || standing->isEmpty())
                return 0;

            return chainBytes(*standing) + sShadingBytes;
        };

        VkDeviceSize cost = 0;
        for (const TextureData& texture : arrived)
        {
            switch (texture.mSource)
            {
                case TextureSource::File:
                {
                    if (!texture.firstLevelWithin(mSideLimit).has_value())
                        break;

                    if (texture.mCompleteChain)
                        cost += texture.mBytes.size()
                            + chainBytes(texture.mWidth, texture.mHeight, levelsTo1x1(texture.mWidth, texture.mHeight))
                            + sShadingBytes;
                    else
                        cost += texture.bytesFrom(firstLevelAt(texture, side)) + sShadingBytes;
                    break;
                }

                case TextureSource::SpriteBake:
                    cost += bakeOf(texture.mFrom);
                    break;

                case TextureSource::GroundComposite:
                case TextureSource::GroundGloss:
                    if (ground)
                        cost += chainBytes(Shaders::GROUND_COMPOSITE_EXTENT, Shaders::GROUND_COMPOSITE_EXTENT,
                                    levelsTo1x1(Shaders::GROUND_COMPOSITE_EXTENT, Shaders::GROUND_COMPOSITE_EXTENT))
                            + sShadingBytes;
                    break;

                case TextureSource::StandIn:
                    break;
            }
        }

        return cost;
    }

    Result<Texture, std::string_view> TextureArray::make(
        Batch& batch, const TextureData& texture, const std::uint32_t side, std::string_view name)
    {
        const VkSampler sampler = mSamplers[static_cast<std::size_t>(texture.mWrap)].get();

        switch (texture.mSource)
        {
            case TextureSource::GroundComposite:
            case TextureSource::GroundGloss:
                return Texture::composite(mDevice, batch, texture.mFormat, name);

            case TextureSource::SpriteBake:
            {
                // The source stands, or draws the stand-in: `SceneTextures` names one only where
                // the table holds it live, a live slot is described whenever it arrives, and
                // `write` stands every source ahead of every bake. A bake of a slot that holds
                // nothing is a contract broken and not content. A source drawing the stand-in is
                // baked as the stand-in, as it is drawn.
                contract(texture.mFrom < mSlots.size() && !mSlots[texture.mFrom].isEmpty(),
                    "a sprite light bake names a source that does not stand");
                return Texture::bakeOf(
                    mDevice, batch, mPasses, sampler, standingIn(mSlots[texture.mFrom]), texture.mFormat, name);
            }

            case TextureSource::File:
            {
                // From the first level within the side, and a level further down each time the
                // device has no room — only a file's own levels, so one whose chain the device
                // completes has the one.
                const auto last = static_cast<std::uint32_t>(texture.mLevels.size()) - 1;
                std::uint32_t first = firstLevelAt(texture, side);
                Result<Texture, std::string_view> made = Texture::fromFile(
                    mDevice, batch, mPasses, sampler, texture, first, name, mRegionScratch, MemoryUse::Texture);
                while (!made.isOk() && first < last)
                    made = Texture::fromFile(
                        mDevice, batch, mPasses, sampler, texture, ++first, name, mRegionScratch, MemoryUse::Texture);

                return made;
            }

            case TextureSource::StandIn:
                break;
        }

        broken("a stand-in made rather than drawn as the one the array holds");
    }

    void TextureArray::stand(
        Batch& batch, const TextureData& texture, const std::uint32_t side, std::vector<Refusal>& refused)
    {
        reserveSlot(texture.mSlot);
        Slot& slot = mSlots[texture.mSlot];

        // Named only where a capture or a validation message could read it back. A local,
        // because a slot number is short enough that this never reaches the heap; the one that
        // does is the map's, inside `Texture`.
        std::string name;
        if constexpr (Device::wantsNames())
            name = "texture " + std::to_string(texture.mSlot);

        // Why the slot draws the stand-in, or nothing where the texture stands. A file the device
        // takes at no level is known before anything is made; room is known by asking for it.
        std::string why;
        Texture made;
        if (texture.mSource == TextureSource::File && !texture.firstLevelWithin(mSideLimit).has_value())
            why = pastTheSide(texture, mSideLimit);
        else if (texture.mSource != TextureSource::StandIn)
        {
            Result<Texture, std::string_view> tried = make(batch, texture, side, name);
            if (tried.isOk())
                made = std::move(tried.value());
            else
                why = tried.error();
        }

        // What the slot held is buried and not destroyed: its descriptor is the one a frame in
        // flight bound, and it stays valid until the timeline says nothing reads it.
        slot.mStandIn = made.isEmpty();
        slot.mReduced = texture.mSource == TextureSource::File && !made.isEmpty()
            && (made.getImage().getWidth() < texture.mWidth || made.getImage().getHeight() < texture.mHeight);
        mDevice.getGraveyard().replace(slot.mTexture, std::move(made));

        if (!why.empty())
            refused.push_back(Refusal{ .mKind = Refused::Texture, .mName = std::string(texture.mName), .mWhy = why });

        if ((texture.mSource == TextureSource::GroundComposite || texture.mSource == TextureSource::GroundGloss)
            && !slot.mStandIn)
            mPendingComposites.push_back(PendingComposite{ .mSlot = texture.mSlot,
                .mMaterial = texture.mFrom,
                .mOutput = texture.mSource == TextureSource::GroundGloss ? Shaders::GROUND_COMPOSITE_GLOSS
                                                                         : Shaders::GROUND_COMPOSITE_ALBEDO });

        // How many texels the slot now holds, for `coneLod`, and whether they are the stand-in's,
        // which every reader of an optional map asks — `TEXTURE_STANDS_IN`. Owed to every copy
        // beside the descriptor owed to every set.
        const Image& stood = standingIn(slot).getImage();
        const std::uint32_t texels = stood.getWidth() * stood.getHeight();
        assert(texels < Shaders::TEXTURE_STANDS_IN && "a texel count that reaches the stand-in bit");
        mTexels.write(texture.mSlot) = texels | (slot.mStandIn ? Shaders::TEXTURE_STANDS_IN : 0u);

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
            // gone is what `drop` says is legal. The neutral texel and the stand-in stand beside
            // the array.
            const Texture& held = at == Shaders::TEXTURE_NEUTRAL ? mNeutral : standingIn(mSlots[at]);
            if (held.isEmpty())
                continue;

            const VkSampler sampler = mSamplers[static_cast<std::size_t>(held.getWrap())].get();
            queueWrite(set, Shaders::TEXTURE_BIND_IMAGES, at, held.describe(sampler), mImageScratch, mWriteScratch);
            queueWrite(
                set, Shaders::TEXTURE_BIND_SHADING, at, held.describeShading(sampler), mImageScratch, mWriteScratch);
        }

        updateSets(mDevice, mWriteScratch);

        owed.clear();
    }

    bool TextureArray::bakeComposites(const VkCommandBuffer commands, const GroundCompositePass& pass,
        const FrameSlot slot, const Shaders::GpuTables& tables)
    {
        if (mPendingComposites.empty())
            return false;

        // A chunk's two images next to each other, so the one sum writes both: they arrive as two
        // textures, in one hand-over or in two.
        std::sort(mPendingComposites.begin(), mPendingComposites.end(),
            [](const PendingComposite& a, const PendingComposite& b) { return a.mMaterial < b.mMaterial; });

        bool baked = false;
        const VkDescriptorSet set = getSet(slot);
        for (std::size_t at = 0; at < mPendingComposites.size();)
        {
            const Index material = mPendingComposites[at].mMaterial;
            const Image* albedo = nullptr;
            const Image* gloss = nullptr;
            std::uint32_t outputs = 0;
            // Up to one of each: a chunk queued twice over before a placement baked it is two sums,
            // which is what it was before the pair shared one.
            for (; at < mPendingComposites.size() && mPendingComposites[at].mMaterial == material
                 && (outputs & mPendingComposites[at].mOutput) == 0;
                 ++at)
            {
                // Arrived and since dropped, before any placement baked it: the slot holds nothing,
                // and a bake of nothing is nothing to record.
                const PendingComposite& pending = mPendingComposites[at];
                const Texture& held = mSlots[pending.mSlot].mTexture;
                if (held.isEmpty())
                    continue;

                (pending.mOutput == Shaders::GROUND_COMPOSITE_GLOSS ? gloss : albedo) = &held.getImage();
                outputs |= pending.mOutput;
            }

            if (outputs == 0)
                continue;

            pass.record(commands, set, albedo, gloss,
                Shaders::GroundCompositeConstants{
                    .mMaterials = tables.mMaterials,
                    .mLayers = tables.mLayers,
                    .mMasks = tables.mMasks,
                    .mMaterial = material,
                    .mOutputs = outputs,
                    .mTexels = getTexelsAddress(slot),
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
            if (slot >= mSlots.size())
                continue;

            // Exchanged rather than erased, so the slot stays where it is and the image goes under
            // the frame that may still name it.
            mDevice.getGraveyard().replace(mSlots[slot].mTexture, Texture());
            mSlots[slot].mStandIn = false;
            mSlots[slot].mReduced = false;
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

        for (const Slot& slot : mSlots)
        {
            // Whether it is there and not its size: a slot stands a texture or it does not, and a
            // content file carrying an empty level is a texture that exists.
            if (slot.mTexture.isEmpty())
                continue;

            ++held.mCount;
            held.mBytes += slot.mTexture.getBytes();
            if (slot.mReduced)
                ++held.mReduced;
        }

        return held;
    }
}

#include "fogvolume.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <components/rtx/fogbuilder.hpp>
#include <components/rtx/shaders/fogvolume.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/sky.h>

#include "barriers.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "dispatch.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    FogTile::FogTile(const Device& device)
        : mField(device, Shaders::FOG_FIELD_SIZE, Shaders::FOG_FIELD_SIZE, VK_FORMAT_R8G8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "fog field", Shaders::FOG_FIELD_LEVELS,
            Shaders::FOG_FIELD_SIZE)
        , mSampler(makeContentSampler(device, "fog field"))
    {
        const FogNoise noise = bakeFogNoise();

        // Every level uploaded rather than halved from the one above, because each is stretched
        // back to one spread (`bakeFogNoise`). Seventy-three kilobytes, once.
        std::vector<VkBufferImageCopy> regions;
        regions.reserve(Shaders::FOG_FIELD_LEVELS);
        for (std::uint32_t level = 0; level < Shaders::FOG_FIELD_LEVELS; ++level)
            regions.push_back(VkBufferImageCopy{
                .bufferOffset = noise.mOffsets[level],
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 },
                .imageExtent = { mField.getWidthAt(level), mField.getHeightAt(level), mField.getDepthAt(level) },
            });

        Batch batch(device.getPool());
        uploadImage(batch, mField, std::as_bytes(std::span(noise.mBytes)), regions);
        batch.flush();
    }

    namespace
    {
        /// Half floats, because nothing here holds a quantity that grows and nothing sums these:
        /// the sun's is a product of transmittances, and the integrated pair is bounded by the
        /// transmittance beside it.
        constexpr VkFormat sFormat = FOG_VOLUME_FORMAT;

        /// `TRANSFER_DST` because the constructor empties every one of these, which is what a
        /// history read before anything has written it needs and what an image made over a departed
        /// cell's memory has no other way of getting.
        constexpr VkImageUsageFlags sUsage
            = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        /// What the set holds, which `shaders/fogvolume.h` states for this side and the shaders
        /// that declare the same slots.
        constexpr std::uint32_t sBindings = Shaders::FOG_BINDING_COUNT;

        constexpr std::uint32_t sSampled = Shaders::FOG_SAMPLED_COUNT;

        constexpr bool sampledAt(std::uint32_t binding)
        {
            return binding < sSampled;
        }

        std::uint32_t columnsFor(std::uint32_t pixels)
        {
            return groupsFor(pixels, Shaders::FOG_VOLUME_SCALE);
        }

        /// Sampled where a pass reads and storage where it writes. One image is named twice wherever
        /// both happen, because Vulkan has no one descriptor that is both. One table serves the
        /// layout and the pool that holds two sets of it.
        constexpr std::array<VkDescriptorSetLayoutBinding, sBindings> sLayoutBindings = [] {
            std::array<VkDescriptorSetLayoutBinding, sBindings> bindings{};
            for (std::uint32_t binding = 0; binding < bindings.size(); ++binding)
                bindings[binding] = VkDescriptorSetLayoutBinding{ binding,
                    sampledAt(binding) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    1,
                    // The volume writes these as a dispatch, and the trace samples them from its
                    // ray generation shader and from the closest-hit shaders that shade what a
                    // bounce found.
                    VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
                        | VK_SHADER_STAGE_MISS_BIT_KHR,
                    nullptr };

            return bindings;
        }();
    }

    SetLayout FogVolume::describeLayout(const Device& device)
    {
        return makeSetLayout(device, sLayoutBindings);
    }

    FogVolume::FogVolume(const Device& device, const SetLayout& layout, std::uint32_t width, std::uint32_t height)
        : mColumns(columnsFor(width))
        , mRows(columnsFor(height))
        , mScatter{ Image(device, mColumns, mRows, sFormat, sUsage, "fog scatter 0", 1, Shaders::FOG_VOLUME_SLICES),
            Image(device, mColumns, mRows, sFormat, sUsage, "fog scatter 1", 1, Shaders::FOG_VOLUME_SLICES) }
        , mSunward{ Image(device, mColumns, mRows, sFormat, sUsage, "fog sunward 0", 1, Shaders::FOG_VOLUME_SLICES),
            Image(device, mColumns, mRows, sFormat, sUsage, "fog sunward 1", 1, Shaders::FOG_VOLUME_SLICES) }
        , mLamps(device, mColumns, mRows, sFormat, sUsage, "fog lamps", 1, Shaders::FOG_VOLUME_SLICES)
        , mAir(device, mColumns, mRows, sFormat, sUsage, "fog air", 1, Shaders::FOG_VOLUME_SLICES)
        , mAirSunward(
              device, mColumns, mRows, FOG_SUNWARD_FORMAT, sUsage, "fog air sunward", 1, Shaders::FOG_VOLUME_SLICES)
        , mSlice(device, mColumns, mRows, sFormat, sUsage, "fog slice", 1, Shaders::FOG_VOLUME_SLICES)
        , mSliceSunward(
              device, mColumns, mRows, FOG_SUNWARD_FORMAT, sUsage, "fog slice sunward", 1, Shaders::FOG_VOLUME_SLICES)
        , mColumnDepth(device, mColumns, mRows, FOG_DEPTH_FORMAT,
              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "fog column depth")
        , mColumnMoons(device, mColumns, mRows, FOG_MOONS_FORMAT,
              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "fog column moons", 1, Shaders::MOON_COUNT)
        , mSampler(makeTargetSampler(device, "fog volume"))
        , mSets(device, sLayoutBindings, layout.get(), sParities)
    {
        // Sampled from `GENERAL` rather than moved to a read-only layout, for the reason
        // `BloomPass` gives: these are written as storage images and read as sampled ones a
        // dispatch apart, and `GENERAL` is the one layout both accesses are legal from.
        for (std::size_t parity = 0; parity < sParities; ++parity)
        {
            const std::size_t written = parity;
            const std::size_t history = 1 - parity;

            std::array<const Image*, sBindings> named{};
            named[Shaders::BIND_FOG_WAS_SCATTER] = &mScatter[history];
            named[Shaders::BIND_FOG_WAS_SUNWARD] = &mSunward[history];
            named[Shaders::BIND_FOG_SCATTER] = &mScatter[written];
            named[Shaders::BIND_FOG_SUNWARD] = &mSunward[written];
            named[Shaders::BIND_FOG_LAMPS] = &mLamps;
            named[Shaders::BIND_FOG_AIR] = &mAir;
            named[Shaders::BIND_FOG_AIR_SUNWARD] = &mAirSunward;
            named[Shaders::BIND_FOG_SLICE] = &mSlice;
            named[Shaders::BIND_FOG_SLICE_SUNWARD] = &mSliceSunward;
            named[Shaders::BIND_FOG_SCATTER_TARGET] = &mScatter[written];
            named[Shaders::BIND_FOG_SUNWARD_TARGET] = &mSunward[written];
            named[Shaders::BIND_FOG_LAMPS_TARGET] = &mLamps;
            named[Shaders::BIND_FOG_AIR_TARGET] = &mAir;
            named[Shaders::BIND_FOG_AIR_SUNWARD_TARGET] = &mAirSunward;
            named[Shaders::BIND_FOG_SLICE_TARGET] = &mSlice;
            named[Shaders::BIND_FOG_SLICE_SUNWARD_TARGET] = &mSliceSunward;
            named[Shaders::BIND_FOG_COLUMN_DEPTH] = &mColumnDepth;
            named[Shaders::BIND_FOG_COLUMN_MOONS] = &mColumnMoons;

            DescriptorWrites<sBindings> writes(mSets.get(parity));
            for (std::uint32_t binding = 0; binding < sBindings; ++binding)
                if (sampledAt(binding))
                    writes.image(binding, named[binding]->describeSampled(mSampler.get()),
                        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                else
                    writes.image(binding, named[binding]->describeStorage());

            updateSets(device, writes.get());
        }

        // Emptied and in `GENERAL` from the moment they exist: `begin` does not discard the point
        // pair, so the first frame reads a history nothing has written, and over a suballocator's
        // range that is a departed image's bits rather than the driver's zeroed pages.
        device.getPool().submitAndWait([&](VkCommandBuffer commands) {
            constexpr VkClearColorValue nothing{ .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };

            for (const Image* image : { &mScatter[0], &mScatter[1], &mSunward[0], &mSunward[1], &mLamps, &mAir,
                     &mAirSunward, &mSlice, &mSliceSunward, &mColumnDepth, &mColumnMoons })
                image->clear(commands, Use::sUndefined, nothing, Use::sAnyGeneral);
        });
    }

    void FogVolume::begin(VkCommandBuffer commands, const FrameSlot trace) const
    {
        const std::size_t written = trace.get();

        // Discarded, because every texel of it is written before any is read; the other half of
        // the pair is this frame's history and survives. The point pair, the lamps and the column
        // images are written by the two launches, the integrated ones by the dispatch after them.
        // The last frame's readers are behind the head barrier `CommandPool::begin` recorded — and
        // so is the launch that wrote the history, which is why the history takes no barrier of
        // its own: it rests in `GENERAL`, and the head barrier made the write visible to every
        // read after it.
        Barriers barriers(commands);
        for (const Image* image : { &mScatter[written], &mSunward[written], &mLamps, &mColumnDepth, &mColumnMoons })
            barriers.add(image->describeTransition(Use::sUndefined, Use::sTraceWrite));
        for (const Image* image : { &mAir, &mAirSunward, &mSlice, &mSliceSunward })
            barriers.add(image->describeTransition(Use::sUndefined, Use::sComputeWrite));

        barriers.flush();
    }

    void FogVolume::depthTaken(VkCommandBuffer commands) const
    {
        // The depth is loaded by the scatter launch, by the integrate dispatch and by the trace,
        // so the one hand-over names both stages; the moons by the scatter launch alone.
        Barriers barriers(commands);
        barriers.add(mColumnDepth.describeTransition(Use::sTraceWrite, Use::sShaderStorageRead));
        barriers.add(mColumnMoons.describeTransition(Use::sTraceWrite, Use::sTraceRead));
        barriers.flush();
    }

    void FogVolume::scattered(VkCommandBuffer commands, const FrameSlot trace) const
    {
        const std::size_t written = trace.get();

        // `GENERAL` to `GENERAL`, so what this orders is the writes against the reads and nothing
        // else. Against the trace as well as the integrate pass, because a puff of smoke reads two
        // of these at a point (`puffLight`).
        Barriers barriers(commands);
        for (const Image* image : { &mScatter[written], &mSunward[written], &mLamps })
            barriers.add(image->describeTransition(Use::sTraceWrite, Use::sShaderSample));

        barriers.flush();
    }

    void FogVolume::handOver(VkCommandBuffer commands) const
    {
        Barriers barriers(commands);
        for (const Image* image : { &mAir, &mAirSunward, &mSlice, &mSliceSunward })
            barriers.add(image->describeTransition(Use::sComputeWrite, Use::sShaderSample));

        barriers.flush();
    }
}

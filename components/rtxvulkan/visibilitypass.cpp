#include "visibilitypass.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <components/rtx/bluenoise.hpp>
#include <components/rtx/lightgrid.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/parallel.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/shaders/bindings.h>
#include <components/rtx/shaders/hosttypes.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/sky.h>
#include <components/rtx/shaders/wave.h>
#include <components/rtx/specularalbedo.hpp>
#include <components/rtx/wavecascade.hpp>

#include "barriers.hpp"
#include "buffer.hpp"
#include "commands.hpp"
#include "device.hpp"
#include "dispatch.hpp"
#include "fogvolume.hpp"
#include "gbuffer.hpp"
#include "gputimer.hpp"
#include "handles.hpp"
#include "imageuse.hpp"
#include "pipeline.hpp"
#include "ripplepass.hpp"
#include "scenebuffers.hpp"
#include "spritebin.hpp"
#include "validation.hpp"
#include "wavepass.hpp"

namespace Rtx
{
    namespace
    {
        /// Whether every table has an address, and each is aligned as the reference that reads it
        /// declares. Debug-only, through the assert that calls it.
        [[maybe_unused]] bool everyTableAddressed(const Shaders::GpuTables& tables)
        {
            const auto at
                = [](std::uint64_t address, std::uint32_t align) { return address != 0 && address % align == 0; };

            return at(tables.mNormalBlocks, Shaders::TABLE_ALIGN_BLOCKS)
                && at(tables.mTangentBlocks, Shaders::TABLE_ALIGN_BLOCKS)
                && at(tables.mTexCoordBlocks, Shaders::TABLE_ALIGN_BLOCKS)
                && at(tables.mColourBlocks, Shaders::TABLE_ALIGN_BLOCKS)
                && at(tables.mIndexBlocks, Shaders::TABLE_ALIGN_BLOCKS) && at(tables.mMeshes, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mInstances, Shaders::TABLE_ALIGN_ROWS) && at(tables.mMaterials, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mLayers, Shaders::TABLE_ALIGN_LAYERS) && at(tables.mMasks, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mLights, Shaders::TABLE_ALIGN_ROWS) && at(tables.mLightList, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mBlueNoise, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mSpecularAlbedo, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mSprites, Shaders::TABLE_ALIGN_ROWS) && at(tables.mEmitters, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mTextureTexels, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mSpriteTileList, Shaders::TABLE_ALIGN_ROWS);
        }

        /// The word `lib/variants.glsl` is compiled with as `REORDER`.
        std::uint32_t reorderWordOf(const Reorder reorder)
        {
            switch (reorder)
            {
                case Reorder::Shader:
                    return Shaders::REORDER_SHADER;
                case Reorder::Texture:
                    return Shaders::REORDER_TEXTURE;
                case Reorder::None:
                    break;
            }

            return Shaders::REORDER_NONE;
        }

        /// Every stage on every binding, because one description of set zero serves the trace's
        /// five shaders and the fog volume's dispatch.
        constexpr auto sStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR
            | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
        constexpr auto sStorage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        /// What each hit record carries: for every closest-hit shader in turn, an eye's run of
        /// layers per eye, which is how a stage is told which eye cast the ray and which layer of
        /// the peel it stands at. `hitRecordTable` is the one statement of it.
        const auto sHitRecords = Shaders::hitRecordTable();

        /// The one hit module under its three settings, in `MaterialKind` order, which is the
        /// order traversal indexes them by: which albedo `resolve` may build, and whether the hit
        /// is shaded as water. Constants five and six, after the frame's tuple.
        constexpr std::array<std::uint32_t, 2> sSurfaceHit{ 0u, 0u };
        constexpr std::array<std::uint32_t, 2> sTerrainHit{ 1u, 0u };
        constexpr std::array<std::uint32_t, 2> sWaterHit{ 0u, 1u };

        /// Whether a deck that is drawn names a sheet at both ends of its blend. The shader mixes
        /// the two unconditionally — `CloudDeck::mBlend` — so a deck with a sheet and no sheet
        /// ahead is a read of the bindless array at a slot nothing holds, which loses the device.
        /// Debug-only, through the assert that calls it.
        [[maybe_unused]] bool deckNamesBothSheets(const Shaders::CloudDeck& deck)
        {
            return deck.mTexture == Shaders::NO_TEXTURE || deck.mNext != Shaders::NO_TEXTURE;
        }

        /// The structure, the hit counter, the frame itself, the sea and the fog's field, in the
        /// order the shader declares them. The tables a hit reads are in `GpuTables`; the channels
        /// the trace writes are `GBuffer`'s set.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::BIND_COUNT> sBindings = [] {
            std::array<VkDescriptorSetLayoutBinding, Shaders::BIND_COUNT> declared{};
            declared[Shaders::BIND_SCENE] = VkDescriptorSetLayoutBinding{ Shaders::BIND_SCENE,
                VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, sStages };

            // The two storage buffers left: every table the shader reads travels as an address in
            // the frame block, and neither the hit counter — a harness facility — nor the glare
            // fader's query has a table to ride in.
            declared[Shaders::BIND_COUNTS] = VkDescriptorSetLayoutBinding{ Shaders::BIND_COUNTS, sStorage, 1, sStages };
            declared[Shaders::BIND_SUN_GLARE]
                = VkDescriptorSetLayoutBinding{ Shaders::BIND_SUN_GLARE, sStorage, 1, sStages };

            declared[Shaders::BIND_FRAME]
                = VkDescriptorSetLayoutBinding{ Shaders::BIND_FRAME, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sStages };

            for (const std::uint32_t binding : { Shaders::BIND_WAVE_SURFACE, Shaders::BIND_WAVE_CURVATURE })
                declared[binding] = VkDescriptorSetLayoutBinding{ binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    Shaders::WAVE_CASCADES, sStages };

            for (const std::uint32_t binding : { Shaders::BIND_RIPPLE_SURFACE, Shaders::BIND_RIPPLE_CURVATURE })
                declared[binding]
                    = VkDescriptorSetLayoutBinding{ binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sStages };

            // One volume rather than a cascade of tiles: the air has no near band and no far one, it
            // has a field read at three scales.
            declared[Shaders::BIND_FOG_FIELD] = VkDescriptorSetLayoutBinding{ Shaders::BIND_FOG_FIELD,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sStages };

            // The frame as shown, for the one launch that composites the puffs over it. In this
            // set because that launch reads everything else in it — the block, the bin through
            // it, the air — and one layout serves every pipeline the set is pushed for.
            declared[Shaders::BIND_SHOWN]
                = VkDescriptorSetLayoutBinding{ Shaders::BIND_SHOWN, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, sStages };

            return declared;
        }();

        /// A table made once on the host — the sampler's tile, the lobe's integrals — for the life
        /// of the pass, in a submit of its own.
        Buffer uploadOnce(const Device& device, std::span<const float> values, std::string_view name)
        {
            Batch batch(device.getPool());
            Buffer table = uploadBuffer(batch, values, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, name);
            batch.flush();
            return table;
        }
    }

    // The hit table is the material kinds, in their own order. Traversal reads an instance's
    // shader-table offset to pick the shader, and `SceneAcceleration::placeRow` writes that offset
    // as the kind itself — so a record out of order would shade every chunk of ground as a pane of
    // glass, and nothing would say so.
    static_assert(static_cast<std::uint32_t>(MaterialKind::Surface) == 0);
    static_assert(static_cast<std::uint32_t>(MaterialKind::Terrain) == 1);
    static_assert(static_cast<std::uint32_t>(MaterialKind::Water) == 2);

    VisibilityVariant VisibilityVariant::resolve(
        const Shaders::VisibilityConstants& frame, const bool water, const bool mapped)
    {
        // A moon that is drawn and a moon that lights are two facts, and the sky needs the first
        // where no surface asks for the second: the game fades both out over the hours around dawn,
        // and a disc still on its way down lights nothing.
        bool moons = false;
        for (const Shaders::MoonDisc& moon : frame.mMoons)
            moons = moons || moon.mAlpha > 0.0f || moon.mSource.mIrradiance != Shaders::vec3();

        return VisibilityVariant{
            // Nought exactly where the sun is not up, and it fades to that across dusk rather than
            // stepping — `VisibilityConstants::mSun`'s irradiance says why there is no second
            // field.
            .mSun = frame.mSun.mIrradiance != Shaders::vec3(),
            .mMoons = moons,

            // Either half is water in the frame: a surface the eye can meet, or a level it can be
            // under. A cell with a level and no surface is one the eye can still be submerged in.
            .mSea = water || !std::isinf(frame.mWaterLevel),
            .mMaps = mapped,
        };
    }

    std::uint32_t VisibilityVariant::index() const
    {
        return (mSun ? 1u : 0u) | (mMoons ? 2u : 0u) | (mSea ? 4u : 0u) | (mMaps ? 8u : 0u);
    }

    std::string VisibilityVariant::describe(const std::string_view kernel) const
    {
        std::string name(kernel);
        if (mSun)
            name += " sun";
        if (mMoons)
            name += " moons";
        if (mSea)
            name += " sea";
        if (mMaps)
            name += " maps";
        return name;
    }

    VisibilityPass::VisibilityPass(const Device& device, const std::filesystem::path& shaderDirectory,
        const SetLayout& textureLayout, const SetLayout& channelLayout, const SetLayout& volumeLayout, bool counting,
        const bool specialize, const Reorder reorder)
        : mDevice(device)
        , mBlueNoise(uploadOnce(device, BlueNoise::shared().getValues(), "blue noise"))
        , mSpecularAlbedo(uploadOnce(device, SpecularAlbedo::shared().getValues(), "specular albedo"))
        , mConstants(Buffer::deviceLocal(device, sizeof(Shaders::VisibilityConstants),
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "frame constants"))
        , mCounting(counting ? 1u : 0u)
        , mReorder(reorderWordOf(reorder))
        , mSpecialize(specialize)
        , mChannelLayout(channelLayout.get())
        , mVolumeLayout(volumeLayout.get())
    {
        compileEvery(shaderDirectory, textureLayout.get());
    }

    void VisibilityPass::compileEvery(const std::filesystem::path& shaders, VkDescriptorSetLayout textureLayout)
    {
        const std::filesystem::path depth = shaders / "fogdepth.rgen.spv";
        const std::filesystem::path scatter = shaders / "fogscatter.rgen.spv";
        const std::filesystem::path integrate = shaders / "fogintegrate.comp.spv";
        const std::filesystem::path spriteComposite = shaders / "spritecomposite.rgen.spv";
        const std::filesystem::path raygen = shaders / "visibility.rgen.spv";
        const std::filesystem::path anyHit = shaders / "visibility.rahit.spv";
        const std::array<std::filesystem::path, Shaders::MISS_RECORD_COUNT> miss{ shaders / "visibility.rmiss.spv" };
        const std::filesystem::path hitModule = shaders / "visibilityhit.rchit.spv";
        const std::array<HitShader, Shaders::HIT_SHADER_COUNT> hit{
            HitShader{ .mModule = hitModule, .mSpecialization = sSurfaceHit },
            HitShader{ .mModule = hitModule, .mSpecialization = sTerrainHit },
            HitShader{ .mModule = hitModule, .mSpecialization = sWaterHit },
        };

        // No tuple and no specialization, because it reads what the pass before it wrote and
        // has no opinion about the sky. Made here rather than among the table below so that the
        // table stays one entry per tuple.
        mDepthPipeline = std::make_unique<TracePipeline>(
            mDevice, sBindings, sharedSets(textureLayout), TraceShaders{ .mRaygen = depth }, "fog depth");
        mIntegratePipeline = std::make_unique<ComputePipeline>(
            mDevice, sBindings, 0, sharedSets(textureLayout), integrate, "fog integrate");
        mSpriteCompositePipeline = std::make_unique<TracePipeline>(mDevice, sBindings, sharedSets(textureLayout),
            TraceShaders{ .mRaygen = spriteComposite, .mRaygenConstantBytes = sizeof(Shaders::PuffConstants) },
            "sprite composite");
        mSpriteShelterPipeline = std::make_unique<TracePipeline>(mDevice, sBindings, sharedSets(textureLayout),
            TraceShaders{ .mRaygen = shaders / "spriteshelter.rgen.spv" }, "sprite shelter");

        /// One kernel to make: which tuple, and which of the two modules.
        struct Wanted
        {
            VisibilityVariant mVariant;
            bool mVolume = false;
        };

        std::vector<Wanted> wanted;
        wanted.reserve(2 * VisibilityVariant::sCount);

        // Every tuple, or the full one alone, which every frame then runs on: its constants are
        // all true and the shaders' own tests answer the rest. The froxels' launch once per tuple
        // without maps, which is what `scatterPipelineFor` asks for.
        for (const bool sun : { false, true })
            for (const bool moons : { false, true })
                for (const bool sea : { false, true })
                    for (const bool maps : { false, true })
                    {
                        if (!mSpecialize && !(sun && moons && sea && maps))
                            continue;

                        const VisibilityVariant variant{ .mSun = sun, .mMoons = moons, .mSea = sea, .mMaps = maps };
                        wanted.push_back(Wanted{ .mVariant = variant });
                        if (!maps || !mSpecialize)
                            wanted.push_back(Wanted{ .mVariant = variant, .mVolume = true });
                    }

        const std::thread::id caller = std::this_thread::get_id();

        // So that a hand's validation error reaches whoever asked for these pipelines. The
        // layers report on the thread that made the call, and the log files by thread because the
        // test binary runs tests in parallel against one of them — an error left filed under a
        // hand is one nobody ever collects.
        runInParallel(
            wanted.size(), [caller] { return AdoptedThread(caller); },
            [&](const std::size_t at) {
                const VisibilityVariant variant = wanted[at].mVariant;
                const bool volume = wanted[at].mVolume;

                // One word per `constant_id`, in the order `lib/variants.glsl` declares them. The
                // volume traces no primary ray and so adds no miss, but its froxels are a boundary
                // the finiteness count watches, so it counts under the same word as the trace.
                const std::array<std::uint32_t, 6> specialization{ mCounting, variant.mSun ? 1u : 0u,
                    variant.mMoons ? 1u : 0u, variant.mSea ? 1u : 0u, mReorder, variant.mMaps ? 1u : 0u };

                if (volume)
                    mScatterPipelines[variant.index()]
                        = std::make_unique<TracePipeline>(mDevice, sBindings, sharedSets(textureLayout),
                            TraceShaders{ .mRaygen = scatter }, variant.describe("fog scatter"), specialization);
                else
                    mPipelines[variant.index()]
                        = std::make_unique<TracePipeline>(mDevice, sBindings, sharedSets(textureLayout),
                            TraceShaders{
                                .mRaygen = raygen,
                                .mMiss = miss,
                                .mHit = hit,
                                .mHitRecordsPerShader = Shaders::HIT_RECORDS_PER_SHADER,
                                .mHitRecordData = std::as_bytes(std::span(sHitRecords)),
                                .mAnyHit = anyHit,
                            },
                            variant.describe("visibility"), specialization);
            });
    }

    std::uint32_t VisibilityPass::slotOf(const VisibilityVariant variant) const
    {
        return mSpecialize ? variant.index() : VisibilityVariant{}.index();
    }

    const TracePipeline& VisibilityPass::pipelineFor(const VisibilityVariant variant) const
    {
        const std::unique_ptr<TracePipeline>& held = mPipelines[slotOf(variant)];
        assert(held != nullptr && "a tuple `compileEvery` did not make");

        return *held;
    }

    const TracePipeline& VisibilityPass::scatterPipelineFor(VisibilityVariant variant) const
    {
        variant.mMaps = false;
        const std::unique_ptr<TracePipeline>& held = mScatterPipelines[slotOf(variant)];
        assert(held != nullptr && "a tuple `compileEvery` made no scatter kernel for");

        return *held;
    }

    SharedSetLayouts VisibilityPass::sharedSets(VkDescriptorSetLayout textureLayout) const
    {
        return SharedSetLayouts{ .mTextures = textureLayout, .mChannels = mChannelLayout, .mVolume = mVolumeLayout };
    }

    void VisibilityPass::writeConstants(VkCommandBuffer commands, const Shaders::VisibilityConstants& described) const
    {
        // A few hundred bytes, so an inline write that runs in queue order, against the launches
        // and the dispatch that read the block as a uniform.
        mConstants.updateInline(commands, Use::sBufferUniformRead, std::as_bytes(std::span(&described, 1)));
    }

    void VisibilityPass::pushInputs(
        VkCommandBuffer commands, const Pipeline& pipeline, const VisibilityInputs& inputs) const
    {
        assert(inputs.mChannels != nullptr && inputs.mCounts != nullptr && "a launch handed no channels or no census");
        const GBuffer& buffer = *inputs.mChannels;
        const Buffer& counts = *inputs.mCounts;

        const VkWriteDescriptorSetAccelerationStructureKHR sceneWrite{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .accelerationStructureCount = 1,
            .pAccelerationStructures = &inputs.mScene,
        };

        // The tiles' widths come off the pass that built them, so what the shader divides by is
        // what is actually bound rather than a second statement of the same table. Sampled from
        // `GENERAL` rather than moved to a read-only layout, for the reason `BloomPass` gives:
        // these are written as storage images and read as sampled ones a few dispatches apart, and
        // `GENERAL` is the one layout both accesses are legal from.
        std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES> surfaces{};
        std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES> curvatures{};
        for (std::size_t cascade = 0; cascade < Shaders::WAVE_CASCADES; ++cascade)
        {
            const VkSampler sampler = inputs.mWaves->getSampler();
            surfaces[cascade] = inputs.mWaves->getSurface(cascade).describeSampled(sampler);
            curvatures[cascade] = inputs.mWaves->getCurvature(cascade).describeSampled(sampler);
        }

        // Appended in binding order rather than indexed, so a channel added cannot silently move
        // two writes on top of each other; the count is checked below rather than maintained.
        DescriptorWrites<sBindings.size(), 2 * Shaders::WAVE_CASCADES + 4> writes;
        writes.structure(Shaders::BIND_SCENE, sceneWrite);

        // The two buffers still bound: the hit counter, and the frame block every table is reached
        // through. Nothing bound here may be nothing: a null handle at the dispatch is undefined
        // and cost this renderer a device before the layers were asked.
        assert(!counts.isEmpty() && !mConstants.isEmpty() && "an input bound as nothing");
        writes.buffer(Shaders::BIND_COUNTS, counts.describe());
        writes.buffer(Shaders::BIND_FRAME, mConstants.describe(), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

        writes.images(Shaders::BIND_WAVE_SURFACE, surfaces, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writes.images(Shaders::BIND_WAVE_CURVATURE, curvatures, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

        writes.image(Shaders::BIND_FOG_FIELD,
            inputs.mFog->getField().describeSampled(
                inputs.mFog->getSampler(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

        assert(inputs.mShown != nullptr && !inputs.mShown->isEmpty() && "a trace with no frame to show");
        writes.image(Shaders::BIND_SHOWN, inputs.mShown->describeStorage());

        assert(inputs.mRipples != nullptr && "a trace with no ripple field stood for it");
        writes.image(Shaders::BIND_RIPPLE_SURFACE,
            inputs.mRipples->getSurface().describeSampled(inputs.mRipples->getSampler()),
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writes.image(Shaders::BIND_RIPPLE_CURVATURE,
            inputs.mRipples->getCurvature().describeSampled(inputs.mRipples->getSampler()),
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

        assert(
            inputs.mSunGlare != nullptr && !inputs.mSunGlare->isEmpty() && "a trace with no glare query to count into");
        writes.buffer(Shaders::BIND_SUN_GLARE, inputs.mSunGlare->describe());

        // Every binding the layout declares, written exactly once — a shader that grew one and a
        // record that did not is the failure this counts.
        assert(writes.size() == sBindings.size() && "a binding the layout declares was left unwritten");

        pushDescriptors(commands, pipeline, writes.get());

        // The three sets nothing pushes: the bindless textures a scene brought, the channels the
        // trace writes, and the air in front of the camera. Each is written when what it names is
        // made, and bound as it is.
        bindSets(commands, pipeline,
            SharedSetBinds{ .mTextures = inputs.mTextures,
                .mChannels = buffer.getSet(),
                .mVolume = inputs.mFogVolume->getSet(inputs.mTraceSlot) });
    }

    void VisibilityPass::writeFrame(VkCommandBuffer commands, const VisibilityInputs& inputs, const SpriteBin& bin,
        const VkDeviceAddress spriteTileList, const Shaders::VisibilityConstants& constants,
        const bool historyLost) const
    {
        assert(inputs.mWaves != nullptr && "a trace with no sea synthesised for it");
        assert(inputs.mFogVolume != nullptr && "a trace with no air integrated for it");

        Shaders::VisibilityConstants described = constants;

        // A basis of nothing is how this block already says there is no previous frame, so a
        // door or a rebuild is told to every reprojection at once rather than to each of them
        // separately. The frame that carries it reprojects nothing, which is what it is for.
        if (historyLost)
        {
            described.mPreviousForward = Shaders::vec3();
            described.mPreviousRight = Shaders::vec3();
            described.mPreviousUp = Shaders::vec3();
        }

        // The tiles' widths come off the pass that built them, so what the shader divides by is
        // what is actually bound rather than a second statement of the same table.
        for (std::size_t cascade = 0; cascade < Shaders::WAVE_CASCADES; ++cascade)
        {
            described.mWaveExtent[cascade] = inputs.mWaves->getExtent(cascade);
            described.mWaveTexel[cascade] = inputs.mWaves->getTexel(cascade);
        }

        described.mWaveSlope = inputs.mWaves->getSlope();

        const WaveCurvature& curvature = inputs.mWaves->getMoments();
        described.mWaveCurvature = curvature.mWhole;
        std::copy(curvature.mResolved.begin(), curvature.mResolved.end(), std::begin(described.mWaveResolved));

        // And where the lamps were binned, off the tables the placement built, for the same reason.
        const LightGrid& lamps = inputs.mBuffers->getLightGrid();
        described.mLightGrid = Shaders::GpuLightGrid{
            .mOrigin = lamps.getOrigin(),
            .mInverseCell = lamps.getInverseCell(),
            .mSize = lamps.getSize(),
        };

        // And how many froxels stand in front of the camera, off the volume that holds them, so the
        // three shaders that divide by it stop asking the driver for a number the host already has.
        described.mFogColumns = Shaders::uvec2(inputs.mFogVolume->getColumns(), inputs.mFogVolume->getRows());

        // And where every table is. Every address read here names a buffer that is alive when the
        // trace runs, because the placement buried what it displaced in the graveyard and nothing
        // between here and the submit grows a table.
        inputs.mBuffers->describeTables(inputs.mSlot, described.mTables);
        described.mTables.mBlueNoise = mBlueNoise.addressFor();
        described.mTables.mSpecularAlbedo = mSpecularAlbedo.addressFor();
        described.mTables.mIndexBlocks = inputs.mIndexBlocks;
        described.mTables.mPoseBlocks = inputs.mPoseBlocks;
        described.mTables.mPreviousPoseBlocks = inputs.mPreviousPoseBlocks;
        described.mTables.mTextureTexels = inputs.mTextureTexels;

        // The trace's own, shaded and binned for this camera ahead of it, or the list of nothing
        // for a camera that draws no sprites and binned none.
        described.mTables.mSprites = bin.getSpritesAddress();
        described.mTables.mSpriteTileList = spriteTileList;

        // Nothing addressed here may be nothing, and every address must be what its reference
        // claims. A descriptor bound as a null handle cost this renderer a device with no message;
        // an address of nought or one off its claimed alignment is the same mistake one step later,
        // and the device says even less about it.
        assert(everyTableAddressed(described.mTables) && "a table addressed as nothing, or not as its block declares");
        assert(deckNamesBothSheets(described.mClouds) && "a deck drawn from one sheet and no sheet ahead");

        writeConstants(commands, described);
    }

    void VisibilityPass::recordSpriteShelter(const VkCommandBuffer commands, const VisibilityInputs& inputs,
        const Shaders::VisibilityConstants& constants, const std::uint32_t count, GpuTimer* const timer) const
    {
        // Nearly every frame: nothing falls, or what falls is the kind a roof does not stop. A
        // frame with sprites and no shelter pays no launch for it.
        if (constants.mShelterHeight <= 0.0f || count == 0)
            return;

        openZone(timer, commands, "shelter");

        bind(commands, *mSpriteShelterPipeline);
        pushInputs(commands, *mSpriteShelterPipeline, inputs);

        // One invocation a sprite, over the bin's own copy of the list.
        mSpriteShelterPipeline->traceRays(commands, count, 1);

        // The shade reads and writes what this zeroed, from a dispatch.
        handOver(commands, Use::sBufferShaderReadWrite, Use::sBufferComputeReadWrite);

        closeZone(timer, commands);
    }

    void VisibilityPass::record(VkCommandBuffer commands, const VisibilityInputs& inputs,
        const Shaders::VisibilityConstants& constants, GpuTimer* timer) const
    {
        assert(inputs.mChannels != nullptr && "a trace with no channels to write");
        assert(inputs.mChannels->getWidth() >= constants.mCamera.mWidth
            && inputs.mChannels->getHeight() >= constants.mCamera.mHeight);

        assert(inputs.mWaves != nullptr && "a trace with no sea synthesised for it");
        assert(inputs.mFog != nullptr && "a trace with no fog field drawn for it");
        assert(inputs.mFogVolume != nullptr && "a trace with no air integrated for it");
        assert(inputs.mTextures != VK_NULL_HANDLE && "a trace whose texture array named no set");

        // Resolved from the constants this frame is about to be traced with, and from nothing
        // kept between frames: a dusk moves the tuple and a doorway moves it again.
        const VisibilityVariant variant = VisibilityVariant::resolve(constants, inputs.mWater, inputs.mMapped);

        const FrameSlot trace = inputs.mTraceSlot;
        inputs.mFogVolume->begin(commands, trace);

        const TracePipeline& scatter = scatterPipelineFor(variant);

        // Every column the image has and not every column the camera needs. A traced view is
        // drawn into a volume grown to the largest one asked for, and the pixel at its edge
        // interpolates against the column outside it — which has to hold air rather than
        // whatever was there.
        const std::uint32_t columns = inputs.mFogVolume->getColumns();
        const std::uint32_t rows = inputs.mFogVolume->getRows();

        openZone(timer, commands, "air");

        // Where each column's ray stops, before anything is drawn along it. One ray a
        // column, and the froxels of the column keep their draws short of the answer.
        bind(commands, *mDepthPipeline);
        pushInputs(commands, *mDepthPipeline, inputs);

        mDepthPipeline->traceRays(commands, columns, rows);

        inputs.mFogVolume->depthTaken(commands);

        // The set stays pushed across all three launches. Every one of them is addressed through
        // the same layout at the same bind point, so what was pushed for the first is still bound
        // for the others — and pushing set zero again would be six descriptor writes for a pass
        // that reads a handful of images out of another set.
        bind(commands, scatter);

        scatter.traceRays(commands, columns, rows, Shaders::FOG_VOLUME_SLICES);

        closeZone(timer, commands);

        inputs.mFogVolume->scattered(commands, trace);

        openZone(timer, commands, "column");

        // The integrate pass is a dispatch and reads what the launches wrote, so it is handed the
        // set again at its own bind point.
        bind(commands, *mIntegratePipeline);
        pushInputs(commands, *mIntegratePipeline, inputs);

        vkCmdDispatch(commands, groupsFor(columns, Shaders::FOG_COLUMN_WORKGROUP),
            groupsFor(rows, Shaders::FOG_COLUMN_WORKGROUP), 1);

        closeZone(timer, commands);

        inputs.mFogVolume->handOver(commands);

        openZone(timer, commands, "trace");

        const TracePipeline& pipeline = pipelineFor(variant);
        bind(commands, pipeline);

        // One invocation a pixel and no tail, where the dispatch it replaces covered the picture
        // in whole workgroups and had every one of them test whether it had run off the edge.
        pipeline.traceRays(commands, constants.mCamera.mWidth, constants.mCamera.mHeight);

        closeZone(timer, commands);

        // The host's read of the count is ordered by whoever reads it: `renderFrame` records
        // `Buffer::orderForHostRead` after every pass that could add to it, and a picture's count
        // is read by nobody.
    }

    void VisibilityPass::recordSpriteComposite(const VkCommandBuffer commands, const VisibilityInputs& inputs,
        const VkExtent2D shown, const VkExtent2D traced, GpuTimer* const timer) const
    {
        assert(inputs.mShown != nullptr && "a composite over no frame");
        assert(shown.width <= inputs.mShown->getWidth() && shown.height <= inputs.mShown->getHeight()
            && "a picture larger than the image it is drawn into");

        // Its own zone and not the bin's `sprites`, so a report says what the march at the shown
        // extent costs apart from what binning the sprites over the traced one does.
        openZone(timer, commands, "puffs");

        bind(commands, *mSpriteCompositePipeline);
        pushInputs(commands, *mSpriteCompositePipeline, inputs);
        pushConstants(commands, *mSpriteCompositePipeline,
            Shaders::PuffConstants{ .mShownWidth = shown.width, .mShownHeight = shown.height });

        // One invocation a traced pixel, which composites the shown pixels over it —
        // `spritecomposite.rgen` says why.
        mSpriteCompositePipeline->traceRays(commands, traced.width, traced.height);

        closeZone(timer, commands);
    }
}

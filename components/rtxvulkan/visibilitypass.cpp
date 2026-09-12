#include "visibilitypass.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <span>
#include <thread>
#include <vector>

#include <components/rtx/bluenoise.hpp>
#include <components/rtx/lightgrid.hpp>
#include <components/rtx/parallel.hpp>
#include <components/rtx/shaders/bindings.h>

#include "buffer.hpp"
#include "commands.hpp"
#include "dispatch.hpp"
#include "fogvolume.hpp"
#include "gbuffer.hpp"
#include "gputimer.hpp"
#include "scenebuffers.hpp"
#include "validation.hpp"
#include "wavepass.hpp"

namespace Rtx
{
    namespace
    {
        /// How many workgroups cover `extent` columns at `workgroup` of them apiece.
        /// Whether every table has an address, and each is aligned as the reference that reads it
        /// declares. Debug-only, through the assert that calls it.
        [[maybe_unused]] bool everyTableAddressed(const Shaders::GpuTables& tables)
        {
            const auto at
                = [](std::uint64_t address, std::uint32_t align) { return address != 0 && address % align == 0; };

            return at(tables.mNormalBlocks, Shaders::TABLE_ALIGN_BLOCKS)
                && at(tables.mTexCoordBlocks, Shaders::TABLE_ALIGN_BLOCKS)
                && at(tables.mColourBlocks, Shaders::TABLE_ALIGN_BLOCKS)
                && at(tables.mIndexBlocks, Shaders::TABLE_ALIGN_BLOCKS) && at(tables.mMeshes, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mInstances, Shaders::TABLE_ALIGN_ROWS) && at(tables.mMaterials, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mLayers, Shaders::TABLE_ALIGN_LAYERS) && at(tables.mMasks, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mLights, Shaders::TABLE_ALIGN_ROWS) && at(tables.mLightList, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mBlueNoise, Shaders::TABLE_ALIGN_ROWS) && at(tables.mSprites, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mEmitters, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mSpriteTileList, Shaders::TABLE_ALIGN_ROWS);
        }

        /// Every stage on every binding, because one description of set zero serves the trace's
        /// five shaders and the fog volume's dispatch.
        constexpr auto sStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR
            | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
        constexpr auto sStorage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        /// What each hit record carries: for every closest-hit shader in turn, one record per layer
        /// of the peel, which is how a shader is told which layer it stands at.
        constexpr std::size_t sHitRecordCount = Shaders::HIT_SHADER_COUNT * Shaders::HIT_RECORD_LAYERS;
        constexpr std::array<Shaders::HitRecord, sHitRecordCount> sHitRecords = [] {
            std::array<Shaders::HitRecord, sHitRecordCount> records{};
            for (std::size_t record = 0; record < records.size(); ++record)
                records[record].mLayer = static_cast<std::uint32_t>(record % Shaders::HIT_RECORD_LAYERS);
            return records;
        }();

        /// The structure, the hit counter, the frame itself, the sea and the fog's field, in the
        /// order the shader declares them. The tables a hit reads are in `GpuTables`; the channels
        /// the trace writes are `GBuffer`'s set.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::BIND_COUNT> sBindings = [] {
            std::array<VkDescriptorSetLayoutBinding, Shaders::BIND_COUNT> declared{};
            declared[Shaders::BIND_SCENE] = VkDescriptorSetLayoutBinding{ Shaders::BIND_SCENE,
                VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, sStages };

            // The one storage buffer left: every table the shader reads travels as an address in the
            // frame block, and the hit counter is a harness facility with no table to ride in.
            declared[Shaders::BIND_HITS] = VkDescriptorSetLayoutBinding{ Shaders::BIND_HITS, sStorage, 1, sStages };

            declared[Shaders::BIND_FRAME]
                = VkDescriptorSetLayoutBinding{ Shaders::BIND_FRAME, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sStages };

            for (const std::uint32_t binding : { Shaders::BIND_WAVE_SURFACE, Shaders::BIND_WAVE_CURVATURE })
                declared[binding] = VkDescriptorSetLayoutBinding{ binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    Shaders::WAVE_CASCADES, sStages };

            // One volume rather than a cascade of tiles: the air has no near band and no far one, it
            // has a field read at three scales.
            declared[Shaders::BIND_FOG_FIELD] = VkDescriptorSetLayoutBinding{ Shaders::BIND_FOG_FIELD,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sStages };

            return declared;
        }();
    }

    // The hit table is the material kinds, in their own order. Traversal reads an instance's
    // shader-table offset to pick the shader, and `SceneAcceleration::placeRow` writes that offset
    // as the kind itself — so a record out of order would shade every chunk of ground as a pane of
    // glass, and nothing would say so.
    static_assert(static_cast<std::uint32_t>(MaterialKind::Surface) == 0);
    static_assert(static_cast<std::uint32_t>(MaterialKind::Terrain) == 1);
    static_assert(static_cast<std::uint32_t>(MaterialKind::Water) == 2);

    VisibilityVariant VisibilityVariant::resolve(const Shaders::VisibilityConstants& frame, const bool water)
    {
        // A moon that is drawn and a moon that lights are two facts, and the sky needs the first
        // where no surface asks for the second: the game fades both out over the hours around dawn,
        // and a disc still on its way down lights nothing.
        bool moons = false;
        for (const Shaders::MoonDisc& moon : frame.mMoons)
            moons = moons || moon.mAlpha > 0.0f || moon.mIrradiance != Shaders::vec3();

        return VisibilityVariant{
            // Nought exactly where the sun is not up, and it fades to that across dusk rather than
            // stepping — `VisibilityConstants::mSunIrradiance` says why there is no second field.
            .mSun = frame.mSunIrradiance != Shaders::vec3(),
            .mMoons = moons,

            // Either half is water in the frame: a surface the eye can meet, or a level it can be
            // under. A cell with a level and no surface is one the eye can still be submerged in.
            .mSea = water || !std::isinf(frame.mWaterLevel),
        };
    }

    std::uint32_t VisibilityVariant::index() const
    {
        return (mSun ? 1u : 0u) | (mMoons ? 2u : 0u) | (mSea ? 4u : 0u);
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
        return name;
    }

    VisibilityPass::VisibilityPass(const Device& device, Batch& batch, const std::filesystem::path& shaderDirectory,
        VkDescriptorSetLayout textureLayout, const SetLayout& channelLayout, const SetLayout& volumeLayout,
        bool countHits, bool countCrossings)
        : mDevice(device)
        , mBlueNoise(
              uploadBuffer(device, batch, BlueNoise::shared().getValues(), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))
        , mConstants(Buffer::deviceLocal(device, sizeof(Shaders::VisibilityConstants),
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        , mCountHits(countHits ? 1u : 0u)
        , mCountCrossings(countCrossings ? 1u : 0u)
        , mChannelLayout(channelLayout.get())
        , mVolumeLayout(volumeLayout.get())
        , mDepthModule(shaderDirectory / "fogdepth.comp.spv")
        , mScatterModule(shaderDirectory / "fogscatter.comp.spv")
        , mIntegrateModule(shaderDirectory / "fogintegrate.comp.spv")
        , mRaygenModule(shaderDirectory / "visibility.rgen.spv")
        , mAnyHitModule(shaderDirectory / "visibility.rahit.spv")
        , mMissModules{ shaderDirectory / "visibility.rmiss.spv" }
        // In `MaterialKind` order, which is the order traversal indexes them by.
        , mHitModules{ shaderDirectory / "visibilitysurface.rchit.spv", shaderDirectory / "visibilityterrain.rchit.spv",
            shaderDirectory / "visibilitywater.rchit.spv" }
    {
        compileEvery(textureLayout);
    }

    void VisibilityPass::compileEvery(VkDescriptorSetLayout textureLayout)
    {
        // No tuple and no specialization, because it reads what the pass before it wrote and
        // has no opinion about the sky. Made here rather than among the table below so that the
        // table stays one entry per tuple.
        mDepthPipeline = std::make_unique<ComputePipeline>(
            mDevice, sBindings, 0, laterSets(textureLayout), mDepthModule, "fog depth");
        mIntegratePipeline = std::make_unique<ComputePipeline>(
            mDevice, sBindings, 0, laterSets(textureLayout), mIntegrateModule, "fog integrate");

        /// One kernel to make: which tuple, and which of the two modules.
        struct Wanted
        {
            VisibilityVariant mVariant;
            bool mVolume = false;
        };

        std::vector<Wanted> wanted;
        wanted.reserve(2 * VisibilityVariant::sCount);

        for (const bool sun : { false, true })
            for (const bool moons : { false, true })
                for (const bool sea : { false, true })
                {
                    const VisibilityVariant variant{ .mSun = sun, .mMoons = moons, .mSea = sea };
                    wanted.push_back(Wanted{ .mVariant = variant });
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
                // volume traces no primary ray, so it counts none whatever the build asked for;
                // every other constant it takes is the tuple's own.
                const std::array<std::uint32_t, 5> specialization{ volume ? 0u : mCountHits, variant.mSun ? 1u : 0u,
                    variant.mMoons ? 1u : 0u, variant.mSea ? 1u : 0u, volume ? 0u : mCountCrossings };

                if (volume)
                    mScatterPipelines[variant.index()] = std::make_unique<ComputePipeline>(mDevice, sBindings, 0,
                        laterSets(textureLayout), mScatterModule, variant.describe("fog scatter"), specialization);
                else
                    mPipelines[variant.index()]
                        = std::make_unique<TracePipeline>(mDevice, sBindings, laterSets(textureLayout),
                            TraceShaders{
                                .mRaygen = mRaygenModule,
                                .mMiss = mMissModules,
                                .mHit = mHitModules,
                                .mHitRecordsPerShader = Shaders::HIT_RECORD_LAYERS,
                                .mHitRecordData = std::as_bytes(std::span(sHitRecords)),
                                .mAnyHit = mAnyHitModule,
                            },
                            variant.describe("visibility"), specialization);
            });
    }

    const TracePipeline& VisibilityPass::pipelineFor(const VisibilityVariant variant) const
    {
        const std::unique_ptr<TracePipeline>& held = mPipelines[variant.index()];
        assert(held != nullptr && "a tuple `compileEvery` did not make");

        return *held;
    }

    const ComputePipeline& VisibilityPass::scatterPipelineFor(const VisibilityVariant variant) const
    {
        const std::unique_ptr<ComputePipeline>& held = mScatterPipelines[variant.index()];
        assert(held != nullptr && "a tuple `compileEvery` made no scatter kernel for");

        return *held;
    }

    std::array<VkDescriptorSetLayout, 3> VisibilityPass::laterSets(VkDescriptorSetLayout textureLayout) const
    {
        return { textureLayout, mChannelLayout, mVolumeLayout };
    }

    void VisibilityPass::writeConstants(VkCommandBuffer commands, const Shaders::VisibilityConstants& described) const
    {
        // Both directions, because one buffer serves every trace: the write has to wait for the
        // last dispatch that read it and for the last write, and the next dispatch for the write.
        const VkBufferMemoryBarrier2 beforeWrite{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
                | VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = mConstants.getHandle(),
            .size = VK_WHOLE_SIZE,
        };
        const VkDependencyInfo settle{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &beforeWrite,
        };
        vkCmdPipelineBarrier2(commands, &settle);

        // A few hundred bytes, so an inline write that runs in queue order. The specification files
        // `vkCmdUpdateBuffer` under the clear commands, so the stage on either side is the clear's.
        vkCmdUpdateBuffer(commands, mConstants.getHandle(), 0, sizeof(described), &described);

        const VkBufferMemoryBarrier2 written{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = mConstants.getHandle(),
            .size = VK_WHOLE_SIZE,
        };
        const VkDependencyInfo handOver{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &written,
        };
        vkCmdPipelineBarrier2(commands, &handOver);
    }

    void VisibilityPass::pushInputs(VkCommandBuffer commands, VkPipelineBindPoint bindPoint, VkPipelineLayout layout,
        const VisibilityInputs& inputs, const GBuffer& buffer, const Buffer& hitCount, std::uint64_t frame) const
    {
        const VkWriteDescriptorSetAccelerationStructureKHR sceneWrite{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .accelerationStructureCount = 1,
            .pAccelerationStructures = &inputs.mScene,
        };
        // The two buffers still bound: the hit counter, and the frame block every table is reached
        // through.
        const VkDescriptorBufferInfo hitWrite{ hitCount.getHandle(), 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo frameWrite{ mConstants.getHandle(), 0, VK_WHOLE_SIZE };

        // The tiles' widths come off the pass that built them, so what the shader divides by is
        // what is actually bound rather than a second statement of the same table.
        std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES> surfaces{};
        std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES> curvatures{};
        for (std::size_t cascade = 0; cascade < Shaders::WAVE_CASCADES; ++cascade)
        {
            const VkSampler sampler = inputs.mWaves->getSampler();
            surfaces[cascade] = { sampler, inputs.mWaves->getSurface(cascade).getView(), VK_IMAGE_LAYOUT_GENERAL };
            curvatures[cascade] = { sampler, inputs.mWaves->getCurvature(cascade).getView(), VK_IMAGE_LAYOUT_GENERAL };
        }

        // Nothing bound here may be nothing: a null handle at the dispatch is undefined and cost
        // this renderer a device before the layers were asked.
        [[maybe_unused]] const auto bound
            = [](const VkDescriptorBufferInfo& write) { return write.buffer != VK_NULL_HANDLE; };
        assert(bound(hitWrite) && bound(frameWrite) && "an input bound as nothing");

        // Appended rather than indexed, so a channel added cannot silently move two writes on top
        // of each other; the count below is checked rather than maintained.
        std::array<VkWriteDescriptorSet, sBindings.size()> writes{};
        std::uint32_t filled = 0;

        const auto append
            = [&](std::uint32_t binding, VkDescriptorType type, const void* next, const VkDescriptorImageInfo* image,
                  const VkDescriptorBufferInfo* block, std::uint32_t count = 1) {
                  assert(filled < writes.size() && "more descriptor writes than the layout has bindings");
                  writes[filled++] = VkWriteDescriptorSet{
                      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                      .pNext = next,
                      .dstBinding = binding,
                      .descriptorCount = count,
                      .descriptorType = type,
                      .pImageInfo = image,
                      .pBufferInfo = block,
                  };
              };
        const auto appendBuffer = [&](std::uint32_t binding, const VkDescriptorBufferInfo& block) {
            append(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, nullptr, &block);
        };
        const auto appendUniform = [&](std::uint32_t binding, const VkDescriptorBufferInfo& block) {
            append(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, nullptr, &block);
        };
        const auto appendImages
            = [&](std::uint32_t binding, const std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES>& images) {
                  append(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, images.data(), nullptr,
                      Shaders::WAVE_CASCADES);
              };

        // The one write whose payload hangs off `pNext` rather than off a pointer field.
        append(Shaders::BIND_SCENE, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, &sceneWrite, nullptr, nullptr);

        appendBuffer(Shaders::BIND_HITS, hitWrite);
        appendUniform(Shaders::BIND_FRAME, frameWrite);

        // Sampled from `GENERAL` rather than moved to a read-only layout, for the reason
        // `BloomPass` gives: these are written as storage images and read as sampled ones a few
        // dispatches apart, and `GENERAL` is the one layout both accesses are legal from.
        appendImages(Shaders::BIND_WAVE_SURFACE, surfaces);
        appendImages(Shaders::BIND_WAVE_CURVATURE, curvatures);

        const VkDescriptorImageInfo fogWrite{ inputs.mFog->getSampler(), inputs.mFog->getField().getView(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        append(Shaders::BIND_FOG_FIELD, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, nullptr, &fogWrite, nullptr);

        // Every binding the layout declares, written exactly once — a shader that grew one and a
        // record that did not is the failure this counts.
        assert(filled == writes.size() && "a binding the layout declares was left unwritten");

        vkCmdPushDescriptorSet(commands, bindPoint, layout, 0, filled, writes.data());

        // The three sets nothing pushes: the bindless textures a scene brought, the channels the
        // trace writes, and the air in front of the camera. Each is written when what it names is
        // made, and bound as it is.
        const std::array<VkDescriptorSet, 3> sets{ inputs.mTextures, buffer.getSet(),
            inputs.mFogVolume->getSet(frame) };
        vkCmdBindDescriptorSets(
            commands, bindPoint, layout, 1, static_cast<std::uint32_t>(sets.size()), sets.data(), 0, nullptr);
    }

    void VisibilityPass::record(VkCommandBuffer commands, const VisibilityInputs& inputs, const GBuffer& buffer,
        const Buffer& hitCount, const Shaders::VisibilityConstants& constants, const bool historyLost,
        GpuTimer* timer) const
    {
        assert(buffer.getWidth() >= constants.mCamera.mWidth && buffer.getHeight() >= constants.mCamera.mHeight);

        assert(inputs.mWaves != nullptr && "a trace with no sea synthesised for it");
        assert(inputs.mFog != nullptr && "a trace with no fog field drawn for it");
        assert(inputs.mFogVolume != nullptr && "a trace with no air integrated for it");
        assert(inputs.mTextures != VK_NULL_HANDLE && "a trace whose texture array named no set");

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
        described.mTables.mBlueNoise = mBlueNoise.getDeviceAddress();
        described.mTables.mIndexBlocks = inputs.mIndexBlocks;
        if (inputs.mSpriteList != 0)
            described.mTables.mSpriteTileList = inputs.mSpriteList;

        // Nothing addressed here may be nothing, and every address must be what its reference
        // claims. A descriptor bound as a null handle cost this renderer a device with no message;
        // an address of nought or one off its claimed alignment is the same mistake one step later,
        // and the device says even less about it.
        assert(everyTableAddressed(described.mTables) && "a table addressed as nothing, or not as its block declares");

        writeConstants(commands, described);

        // Resolved from the constants this frame is about to be traced with, and from nothing
        // kept between frames: a dusk moves the tuple and a doorway moves it again.
        const VisibilityVariant variant = VisibilityVariant::resolve(constants, inputs.mWater);

        inputs.mFogVolume->begin(commands, constants.mFrame);

        const ComputePipeline& scatter = scatterPipelineFor(variant);

        // Every column the image has and not every column the camera needs. A traced view is
        // drawn into a volume grown to the largest one asked for, and the pixel at its edge
        // interpolates against the column outside it — which has to hold air rather than
        // whatever was there.
        const std::uint32_t columns = inputs.mFogVolume->getColumns();
        const std::uint32_t rows = inputs.mFogVolume->getRows();

        openZone(timer, commands, "air");

        // Where each column's ray stops, before anything is drawn along it. One ray a
        // column, and the froxels of the column keep their draws short of the answer.
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mDepthPipeline->getHandle());
        pushInputs(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mDepthPipeline->getLayout(), inputs, buffer, hitCount,
            constants.mFrame);

        vkCmdDispatch(commands, groupsFor(columns, Shaders::FOG_COLUMN_WORKGROUP),
            groupsFor(rows, Shaders::FOG_COLUMN_WORKGROUP), 1);

        inputs.mFogVolume->depthTaken(commands);

        // The set stays pushed across all three dispatches. Every pipeline here is
        // addressed through the same layout at the same bind point, so what was pushed for the
        // first is still bound for the others — and pushing set zero again would be six
        // descriptor writes for a pass that reads a handful of images out of another set.
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, scatter.getHandle());

        vkCmdDispatch(commands, groupsFor(columns, Shaders::FOG_FROXEL_WORKGROUP_ACROSS),
            groupsFor(rows, Shaders::FOG_FROXEL_WORKGROUP_ACROSS),
            groupsFor(Shaders::FOG_VOLUME_SLICES, Shaders::FOG_FROXEL_WORKGROUP_DEEP));

        closeZone(timer, commands);

        inputs.mFogVolume->scattered(commands, constants.mFrame);

        openZone(timer, commands, "column");

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mIntegratePipeline->getHandle());

        vkCmdDispatch(commands, groupsFor(columns, Shaders::FOG_COLUMN_WORKGROUP),
            groupsFor(rows, Shaders::FOG_COLUMN_WORKGROUP), 1);

        closeZone(timer, commands);

        inputs.mFogVolume->handOver(commands);

        openZone(timer, commands, "trace");

        const TracePipeline& pipeline = pipelineFor(variant);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline.getHandle());
        pushInputs(commands, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline.getLayout(), inputs, buffer, hitCount,
            constants.mFrame);

        // One invocation a pixel and no tail, where the dispatch it replaces covered the picture
        // in whole workgroups and had every one of them test whether it had run off the edge.
        pipeline.traceRays(commands, constants.mCamera.mWidth, constants.mCamera.mHeight);

        closeZone(timer, commands);

        // The count is read on the host after the frame's fence, and a fence makes nothing visible
        // to the host — its access scope holds device access only — so the host's read is named
        // here, where the write is.
        const VkMemoryBarrier2 counted{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        };
        const VkDependencyInfo dependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &counted,
        };
        vkCmdPipelineBarrier2(commands, &dependency);
    }
}

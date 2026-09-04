#include "visibilitypass.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <exception>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <components/rtx/bluenoise.hpp>
#include <components/rtx/lightgrid.hpp>
#include <components/rtx/shaders/bindings.h>

#include "buffer.hpp"
#include "commands.hpp"
#include "fogtile.hpp"
#include "fogvolume.hpp"
#include "gbuffer.hpp"
#include "gputimer.hpp"
#include "scenebuffers.hpp"
#include "wavepass.hpp"

namespace Rtx
{
    namespace
    {
        /// How many workgroups cover `extent` columns at `workgroup` of them apiece.
        std::uint32_t groupsFor(std::uint32_t extent, std::uint32_t workgroup)
        {
            return (extent + workgroup - 1) / workgroup;
        }

        /// Whether every table has an address, and each is aligned as the reference that reads it
        /// declares. Debug-only, through the assert that calls it.
        [[maybe_unused]] bool everyTableAddressed(const Shaders::GpuTables& tables)
        {
            const auto at
                = [](std::uint64_t address, std::uint32_t align) { return address != 0 && address % align == 0; };

            return at(tables.mNormalBlocks, Shaders::TABLE_ALIGN_BLOCKS)
                && at(tables.mTexCoordBlocks, Shaders::TABLE_ALIGN_BLOCKS)
                && at(tables.mIndexBlocks, Shaders::TABLE_ALIGN_BLOCKS) && at(tables.mMeshes, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mInstances, Shaders::TABLE_ALIGN_ROWS) && at(tables.mMaterials, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mLayers, Shaders::TABLE_ALIGN_LAYERS) && at(tables.mMasks, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mLights, Shaders::TABLE_ALIGN_ROWS) && at(tables.mLightList, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mBlueNoise, Shaders::TABLE_ALIGN_ROWS) && at(tables.mSprites, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mEmitters, Shaders::TABLE_ALIGN_ROWS)
                && at(tables.mSpriteTileList, Shaders::TABLE_ALIGN_ROWS);
        }

        /// **Every stage on every binding, because one description of set zero serves both passes.**
        /// The trace is a launch of five shaders and the fog volume is a dispatch, and all of them
        /// read the same tables out of the same pushed set — so the layout they are addressed
        /// through has to be legal for each of them. A hit is resolved in a closest-hit shader, a
        /// cutout is tested in an any-hit shader and the sky is drawn in a miss shader, and each of
        /// those reads the instances, the materials and the textures the same way the launch used
        /// to.
        constexpr auto sStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR
            | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
        constexpr auto sStorage = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        /// The structure, the hit counter, the frame itself, the sea and the fog's field, in the
        /// order the shader declares them. The tables a hit reads are not here: `GpuTables` in the
        /// frame block says where they are. The channels the trace writes are not here either:
        /// `GBuffer` says why they have a set of their own.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::BIND_COUNT> sBindings = [] {
            std::array<VkDescriptorSetLayoutBinding, Shaders::BIND_COUNT> declared{};
            declared[Shaders::BIND_SCENE] = VkDescriptorSetLayoutBinding{ Shaders::BIND_SCENE,
                VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, sStages };

            // The one storage buffer left: every table the shader reads travels as an address in the
            // frame block, and the hit counter is a harness facility with no table to ride in.
            declared[Shaders::BIND_HITS] = VkDescriptorSetLayoutBinding{ Shaders::BIND_HITS, sStorage, 1, sStages };

            declared[Shaders::BIND_FRAME]
                = VkDescriptorSetLayoutBinding{ Shaders::BIND_FRAME, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, sStages };

            // Then the two the sea was synthesised into, one descriptor a cascade.
            for (const std::uint32_t binding : { Shaders::BIND_WAVE_SURFACE, Shaders::BIND_WAVE_CURVATURE })
                declared[binding] = VkDescriptorSetLayoutBinding{ binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    Shaders::WAVE_CASCADES, sStages };

            // And the one the fog's field was drawn into, which is one volume rather than a cascade
            // of tiles: the air has no near band and no far one, it has a field read at three scales.
            declared[Shaders::BIND_FOG_FIELD] = VkDescriptorSetLayoutBinding{ Shaders::BIND_FOG_FIELD,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sStages };

            return declared;
        }();
    }

    static_assert(static_cast<std::uint32_t>(Reorder::Off) == Shaders::REORDER_OFF);
    static_assert(static_cast<std::uint32_t>(Reorder::Hit) == Shaders::REORDER_HIT);
    static_assert(static_cast<std::uint32_t>(Reorder::Hint) == Shaders::REORDER_HINT);
    static_assert(static_cast<std::uint32_t>(Reorder::Both) == Shaders::REORDER_BOTH);

    // **The hit table is the material kinds, in their own order.** Traversal reads an instance's
    // shader-table offset to pick the shader, and `SceneAcceleration::placeRow` writes that offset
    // as the kind itself — so a record out of order would shade every chunk of ground as a pane of
    // glass, and nothing would say so.
    static_assert(static_cast<std::uint32_t>(MaterialKind::Surface) == 0);
    static_assert(static_cast<std::uint32_t>(MaterialKind::Terrain) == 1);
    static_assert(static_cast<std::uint32_t>(MaterialKind::Water) == 2);

    VisibilityVariant VisibilityVariant::resolve(const Shaders::VisibilityConstants& frame, const bool water)
    {
        // **A moon that is drawn and a moon that lights are two facts**, and the sky needs the first
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
            .mUniformFog = frame.mFogUniform >= 1.0f,
        };
    }

    std::uint32_t VisibilityVariant::index() const
    {
        return (mSun ? 1u : 0u) | (mMoons ? 2u : 0u) | (mSea ? 4u : 0u) | (mUniformFog ? 8u : 0u);
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
        if (mUniformFog)
            name += " even-air";
        return name;
    }

    VisibilityPass::VisibilityPass(const Device& device, Batch& batch, const std::filesystem::path& shaderDirectory,
        VkDescriptorSetLayout textureLayout, const SetLayout& channelLayout, const SetLayout& volumeLayout,
        bool countHits, bool countCrossings, Reorder reorder)
        : mDevice(device)
        , mBlueNoise(
              uploadBuffer(device, batch, BlueNoise::shared().getValues(), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT))
        , mConstants(Buffer::deviceLocal(device, sizeof(Shaders::VisibilityConstants),
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        , mCountHits(countHits ? 1u : 0u)
        , mCountCrossings(countCrossings ? 1u : 0u)
        , mReorder(reorder)
        , mChannelLayout(channelLayout.getHandle())
        , mVolumeLayout(volumeLayout.getHandle())
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
        // **No tuple and no specialization**, because it reads what the pass before it wrote and
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
        wanted.reserve(VisibilityVariant::sCount + VisibilityVariant::sCount / 2);

        for (const bool sun : { false, true })
            for (const bool moons : { false, true })
                for (const bool sea : { false, true })
                    for (const bool evenAir : { false, true })
                    {
                        const VisibilityVariant variant{
                            .mSun = sun, .mMoons = moons, .mSea = sea, .mUniformFog = evenAir
                        };
                        wanted.push_back(Wanted{ .mVariant = variant });

                        // A room reads the closed form, so it needs no volume and gets no kernel.
                        if (!evenAir)
                            wanted.push_back(Wanted{ .mVariant = variant, .mVolume = true });
                    }

        std::atomic<std::size_t> next{ 0 };
        std::mutex kept;
        std::exception_ptr failed;

        const auto compile = [&] {
            for (std::size_t at = next++; at < wanted.size(); at = next++)
            {
                try
                {
                    const VisibilityVariant variant = wanted[at].mVariant;
                    const bool volume = wanted[at].mVolume;

                    // One word per `constant_id`, in the order `lib/variants.glsl` declares them.
                    // The volume traces no primary ray and reorders nothing, so it counts none and
                    // sorts none whatever the build asked for; every other constant it takes is the
                    // tuple's own, and no even air ever reaches the volume's table.
                    const std::array<std::uint32_t, 7> specialization{ volume ? 0u : mCountHits, variant.mSun ? 1u : 0u,
                        variant.mMoons ? 1u : 0u, variant.mSea ? 1u : 0u, variant.mUniformFog ? 1u : 0u,
                        volume ? Shaders::REORDER_OFF : static_cast<std::uint32_t>(mReorder),
                        volume ? 0u : mCountCrossings };

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
                                    .mAnyHit = mAnyHitModule,
                                },
                                variant.describe("visibility"), specialization);
                }
                catch (...)
                {
                    // **The first failure and not the last**, so the message names what actually
                    // went wrong rather than whichever thread finished after it.
                    const std::lock_guard<std::mutex> hold(kept);
                    if (failed == nullptr)
                        failed = std::current_exception();
                }
            }
        };

        const std::size_t hands
            = std::max<std::size_t>(1, std::min<std::size_t>(std::thread::hardware_concurrency(), wanted.size()));

        {
            std::vector<std::jthread> compiling;
            compiling.reserve(hands);
            for (std::size_t hand = 0; hand < hands; ++hand)
                compiling.emplace_back(compile);
        }

        if (failed != nullptr)
            std::rethrow_exception(failed);
    }

    const TracePipeline& VisibilityPass::pipelineFor(const VisibilityVariant variant) const
    {
        const std::unique_ptr<TracePipeline>& held = mPipelines[variant.index()];
        assert(held != nullptr && "a tuple `compileEvery` did not make");

        return *held;
    }

    const ComputePipeline* VisibilityPass::scatterPipelineFor(const VisibilityVariant variant) const
    {
        assert((variant.mUniformFog || mScatterPipelines[variant.index()] != nullptr)
            && "a banked air with no scatter kernel made for it");

        return mScatterPipelines[variant.index()].get();
    }

    std::array<VkDescriptorSetLayout, 3> VisibilityPass::laterSets(VkDescriptorSetLayout textureLayout) const
    {
        return { textureLayout, mChannelLayout, mVolumeLayout };
    }

    void VisibilityPass::writeConstants(VkCommandBuffer commands, const Shaders::VisibilityConstants& described) const
    {
        // **Both directions, because one buffer serves every trace.** The write has to wait for the
        // last dispatch that read it — a traced view and the world are two traces — and the next
        // dispatch has to wait for the write. **And for the last write**, which two frames in
        // flight make the frame before's own update of this buffer: a write after a write is a
        // hazard of its own, and the dispatch between them is not what orders it.
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

        // A few hundred bytes, so this is an inline write into the command buffer rather than a
        // staging copy — and being recorded, it runs in queue order with the traces around it. A
        // clear command and not a copy, which is what the stage on either side of it says: the
        // specification files `vkCmdUpdateBuffer` under the clear commands, and a barrier at the
        // copy stage leaves the write outside its scope.
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

        // **The tiles' widths come off the pass that built them**, so what the shader divides by is
        // what is actually bound rather than a second statement of the same table.
        std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES> surfaces{};
        std::array<VkDescriptorImageInfo, Shaders::WAVE_CASCADES> curvatures{};
        for (std::size_t cascade = 0; cascade < Shaders::WAVE_CASCADES; ++cascade)
        {
            const VkSampler sampler = inputs.mWaves->getSampler();
            surfaces[cascade] = { sampler, inputs.mWaves->getSurface(cascade).getView(), VK_IMAGE_LAYOUT_GENERAL };
            curvatures[cascade] = { sampler, inputs.mWaves->getCurvature(cascade).getView(), VK_IMAGE_LAYOUT_GENERAL };
        }

        // **Nothing bound here may be nothing.** A descriptor the shader declares and a null handle
        // is undefined at the dispatch: the driver may fault, may not, and says nothing either way —
        // it cost this renderer a device and five seconds of a wedged process before the layers were
        // asked. Both of these are inputs a caller promises to pass, so a null is a broken promise
        // and not a state to handle. The tables are asked the same question as addresses, in
        // `record`.
        [[maybe_unused]] const auto bound
            = [](const VkDescriptorBufferInfo& write) { return write.buffer != VK_NULL_HANDLE; };
        assert(bound(hitWrite) && bound(frameWrite) && "an input bound as nothing");

        // **Appended rather than indexed.** Every one of these used to name its own slot — channels
        // at `1 + i`, buffers at `i + 8`, then twenty-one through twenty-six by hand — so adding a
        // channel silently moved two buffer writes on top of each other and left the new bindings
        // unwritten. The layout said what was wrong and nothing else did. A cursor cannot make that
        // mistake, and the count below is checked rather than maintained.
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

        // **Sampled from `GENERAL` rather than moved to a read-only layout**, for the reason
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

        // **A basis of nothing is how this block already says there is no previous frame**, so a
        // door or a rebuild is told to every reprojection at once rather than to each of them
        // separately. The frame that carries it reprojects nothing, which is what it is for.
        if (historyLost)
        {
            described.mPreviousForward = Shaders::vec3();
            described.mPreviousRight = Shaders::vec3();
            described.mPreviousUp = Shaders::vec3();
        }

        // **The tiles' widths come off the pass that built them**, so what the shader divides by is
        // what is actually bound rather than a second statement of the same table.
        for (std::size_t cascade = 0; cascade < Shaders::WAVE_CASCADES; ++cascade)
            described.mWaveExtent[cascade] = inputs.mWaves->getExtent(cascade);

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

        // And where every table is, for the same reason. The scene names its own; the two it does
        // not own are the pass's tile and the structure's index blocks.
        //
        // **Every address read here names a buffer that is alive when the trace runs**, because the
        // placement ran before this and buried what it displaced in the graveyard, which holds it
        // until this frame's fence. Nothing between here and the submit grows a table.
        inputs.mBuffers->describeTables(inputs.mSlot, described.mTables);
        described.mTables.mBlueNoise = mBlueNoise.getDeviceAddress();
        described.mTables.mIndexBlocks = inputs.mIndexBlocks;

        // **Nothing addressed here may be nothing, and every address must be what its reference
        // claims.** A descriptor bound as a null handle cost this renderer a device with no message;
        // an address of nought or one off its claimed alignment is the same mistake one step later,
        // and the device says even less about it.
        assert(everyTableAddressed(described.mTables) && "a table addressed as nothing, or not as its block declares");

        writeConstants(commands, described);

        // **Resolved from the constants this frame is about to be traced with**, and from nothing
        // kept between frames: a dusk moves the tuple and a doorway moves it again.
        const VisibilityVariant variant = VisibilityVariant::resolve(constants, inputs.mWater);

        // Taken for writing whether or not it is filled, because the trace binds a descriptor to it
        // either way and a descriptor names the layout its image is in.
        inputs.mFogVolume->begin(commands, constants.mFrame);

        if (const ComputePipeline* scatter = scatterPipelineFor(variant); scatter != nullptr)
        {
            // **Every column the image has and not every column the camera needs.** A traced view is
            // drawn into a volume grown to the largest one asked for, and the pixel at its edge
            // interpolates against the column outside it — which has to hold air rather than
            // whatever was there.
            const std::uint32_t columns = inputs.mFogVolume->getColumns();
            const std::uint32_t rows = inputs.mFogVolume->getRows();

            openZone(timer, commands, "air");

            // **Where each column's ray stops, before anything is drawn along it.** One ray a
            // column, and the froxels of the column keep their draws short of the answer.
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mDepthPipeline->getHandle());
            pushInputs(commands, VK_PIPELINE_BIND_POINT_COMPUTE, mDepthPipeline->getLayout(), inputs, buffer, hitCount,
                constants.mFrame);

            vkCmdDispatch(commands, groupsFor(columns, Shaders::FOG_COLUMN_WORKGROUP),
                groupsFor(rows, Shaders::FOG_COLUMN_WORKGROUP), 1);

            inputs.mFogVolume->depthTaken(commands);

            // **The set stays pushed across all three dispatches.** Every pipeline here is
            // addressed through the same layout at the same bind point, so what was pushed for the
            // first is still bound for the others — and pushing set zero again would be six
            // descriptor writes for a pass that reads a handful of images out of another set.
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, scatter->getHandle());

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
        }

        openZone(timer, commands, "trace");

        const TracePipeline& pipeline = pipelineFor(variant);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline.getHandle());
        pushInputs(commands, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline.getLayout(), inputs, buffer, hitCount,
            constants.mFrame);

        // **One invocation a pixel and no tail**, where the dispatch it replaces covered the picture
        // in whole workgroups and had every one of them test whether it had run off the edge.
        pipeline.traceRays(commands, constants.mCamera.mWidth, constants.mCamera.mHeight);

        closeZone(timer, commands);

        // **The count is read on the host after the frame's fence, and a fence makes nothing
        // visible to the host.** The specification's note under fence signalling says so outright —
        // the access scope of the dependency a fence defines holds device access only — and points
        // at the host access types for the barrier that does. So the host's read is named here,
        // where the write is, the way `SpriteBinPass` names the read of its report. A picture inside
        // the interface traces through this too and nobody reads its count; what that costs is a
        // barrier nothing waits behind.
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

#include <components/rtx/shaders/look.h>

#include "atrouspass.hpp"

#include <array>
#include <cassert>

#include "dispatch.hpp"
#include "gbuffer.hpp"

namespace Rtx
{
    namespace
    {
        /// The channel coming in, the channel going out, the two that say where the edges in the
        /// surface are and the one that says where the edges in the light are. All pushed. Sampled
        /// on the four this pass only reads, because a twenty-five tap gather wants the texture
        /// unit's cache — a few per cent of the cascade — and legal from `VK_IMAGE_LAYOUT_GENERAL`.
        constexpr std::array<VkDescriptorSetLayoutBinding, 5> sBindings{
            computeBinding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            computeBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            computeBinding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        };

        /// Both reads, because a level's inputs are sampled and its target is storage. An image
        /// is each in turn as the levels ping-pong, so a dependency that named one of the two would
        /// leave the other frame's access uncovered.
        constexpr VkAccessFlags2 sReads = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

        /// Three ways of feeding this pass the same taps more cheaply were measured and none pays:
        /// a shared-memory tile with Dolp's permutation, because the pass costs the same per level
        /// whatever the stride and there is no locality to recover; one geometry channel instead of
        /// the guide and the depth, neutral; packing it to eight bytes, worse, because the
        /// octahedral `normalize` over 125 taps costs more than a fetch. What the pass spends is
        /// the two `exp` and the guide tap. A profiler is what the next attempt should start from.

        /// How sharply a tap's normal has to agree with the centre's, and how far off its plane it
        /// may sit. The exponent is SVGF's own; the sigma measures a distance off a plane in pixel
        /// footprints where the paper divides by a depth gradient. Not a setting, because the
        /// reference mode is what will say what to set them to.
        constexpr float sNormalPower = 128.0f;
        constexpr float sPlaneSigma = 2.0f;

        /// How far a tap's brightness may differ from the centre's, in standard deviations of what
        /// the centre has been measuring. SVGF's own, and for the first time there is a variance to
        /// scale it by.
        constexpr float sLuminanceSigma = 4.0f;
    }

    AtrousPass::AtrousPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mDevice(device)
        , mPipeline(
              device, sBindings, sizeof(Shaders::AtrousConstants), {}, shaderDirectory / "atrous.comp.spv", "atrous")
    {
    }

    void AtrousPass::resize(std::uint32_t width, std::uint32_t height)
    {
        if (mScratch != nullptr && mScratch->getWidth() == width && mScratch->getHeight() == height)
            return;

        // `SAMPLED` because a level reads what the level before it wrote, and it reads through
        // the texture unit. See `sBindings`.
        mScratch = std::make_unique<Image>(mDevice, width, height, ATROUS_CHANNEL,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "atrous-scratch");
    }

    const Image& AtrousPass::record(VkCommandBuffer commands, const GBuffer& buffer, const Image& blended,
        const Image& moments, const Image& history, const Shaders::Camera& camera) const
    {
        assert(mScratch != nullptr && "record before resize");
        assert(mScratch->getWidth() >= camera.mWidth && mScratch->getHeight() >= camera.mHeight);
        assert(buffer.getWidth() >= camera.mWidth && buffer.getHeight() >= camera.mHeight);

        // Nothing has written the scratch yet this frame, so the first level may discard it. Every
        // level after reads what the one before wrote, which is what the barriers below order.
        mScratch->transition(commands,
            ImageUse{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, sReads }, Use::sComputeWrite);

        // One assignment and not eight, so the filter's rays and the trace's cannot come to
        // differ: this pass is handed the one struct, and the shader rebuilds the rays with the
        // trace's own `rayAt`.
        Shaders::AtrousConstants level{
            .mCamera = camera,
            .mStep = 1,
            .mNormalPower = sNormalPower,
            .mPlaneSigma = sPlaneSigma,
            .mLuminanceSigma = sLuminanceSigma,
        };

        // Three images take turns and not two, because the first level's answer is the mean the
        // accumulator reads next frame — SVGF's feedback — so the levels after it ping-pong between
        // the blend and the scratch and leave it alone.
        const Image* source = &blended;
        const Image* target = &history;

        for (std::uint32_t pass = 0; pass < Shaders::ATROUS_LEVELS; ++pass)
        {
            if (pass > 0)
            {
                // The level about to run reads what the last one wrote and overwrites what it read,
                // so both channels have to be ordered against it — the second is a write after a
                // read, which needs the stages named and nothing made visible.
                Barriers between(commands);
                for (const Image* image : { source, target })
                    between.add(image->describeTransition(
                        ImageUse{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | sReads },
                        ImageUse{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | sReads }));

                between.flush();
            }

            const std::array<VkDescriptorImageInfo, 5> images{
                VkDescriptorImageInfo{ VK_NULL_HANDLE, source->getView(), VK_IMAGE_LAYOUT_GENERAL },
                VkDescriptorImageInfo{ VK_NULL_HANDLE, target->getView(), VK_IMAGE_LAYOUT_GENERAL },
                VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.get(Channel::Guide).getView(), VK_IMAGE_LAYOUT_GENERAL },
                VkDescriptorImageInfo{ VK_NULL_HANDLE, buffer.get(Channel::Depth).getView(), VK_IMAGE_LAYOUT_GENERAL },
                VkDescriptorImageInfo{ VK_NULL_HANDLE, moments.getView(), VK_IMAGE_LAYOUT_GENERAL },
            };

            const std::array<VkWriteDescriptorSet, 5> writes = imageWrites(images, sBindings);

            level.mStep = 1u << pass;

            dispatch(commands, mPipeline, writes, level, groupsFor(camera.mWidth, Shaders::ATROUS_WORKGROUP),
                groupsFor(camera.mHeight, Shaders::ATROUS_WORKGROUP));

            // The next level reads what this one wrote, and writes whichever of the other two it is
            // not reading — the blend after the first level, and the scratch and the blend by turns
            // after that.
            source = target;
            target = pass == 0 ? &blended : (source == &blended ? mScratch.get() : &blended);
        }

        // The cascade hands over what it wrote, because nothing after it does: with the last level
        // ordered against nothing, the composite ran beside the dispatch still writing it, and two
        // runs of one doll wrote different bytes over a thousand of its pixels.
        source->transition(commands, Use::sComputeWrite,
            ImageUse{ VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, sReads });

        // One swap past the last dispatch, so this is what that dispatch wrote.
        return *source;
    }
}

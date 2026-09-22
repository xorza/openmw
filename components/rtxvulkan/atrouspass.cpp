#include "atrouspass.hpp"

#include <array>
#include <cassert>

#include <components/rtx/frameimage.hpp>
#include <components/rtx/shaders/atrous.h>
#include <components/rtx/shaders/camera.h>
#include <components/rtx/shaders/look.h>

#include "barriers.hpp"
#include "dispatch.hpp"
#include "gbuffer.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    namespace
    {
        /// The channel coming in, the channel going out, the two that say where the edges in the
        /// surface are and the one that says where the edges in the light are. All pushed. Sampled
        /// on the four this pass only reads, because a twenty-five tap gather wants the texture
        /// unit's cache — a few per cent of the cascade — and legal from `VK_IMAGE_LAYOUT_GENERAL`.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::ATROUS_BINDINGS> sBindings{
            computeBinding(Shaders::ATROUS_BIND_SOURCE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            computeBinding(Shaders::ATROUS_BIND_FILTERED, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::ATROUS_BIND_GUIDE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            computeBinding(Shaders::ATROUS_BIND_DEPTH, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            computeBinding(Shaders::ATROUS_BIND_MOMENTS, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
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
    }

    AtrousPass::AtrousPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mPipeline(
            device, sBindings, sizeof(Shaders::AtrousConstants), {}, shaderDirectory / "atrous.comp.spv", "atrous")
    {
    }

    Image AtrousPass::makeScratch(const Device& device, std::uint32_t width, std::uint32_t height)
    {
        // `SAMPLED` because a level reads what the level before it wrote, and it reads through
        // the texture unit. See `sBindings`.
        return Image(device, width, height, ATROUS_CHANNEL, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            "atrous-scratch");
    }

    const Image& AtrousPass::record(VkCommandBuffer commands, const GBuffer& buffer, const Image& blended,
        const Image& moments, const Image& history, const Image& scratch, const Shaders::Camera& camera) const
    {
        assert(!scratch.isEmpty() && "a filter with no scratch to ping-pong through");
        assert(scratch.getWidth() >= camera.mWidth && scratch.getHeight() >= camera.mHeight);
        assert(buffer.getWidth() >= camera.mWidth && buffer.getHeight() >= camera.mHeight);

        // Nothing has written the scratch yet this frame, so the first level may discard it. Every
        // level after reads what the one before wrote, which is what the barriers below order.
        scratch.transition(commands, Use::sUndefined, Use::sComputeWrite);

        // One assignment and not eight, so the filter's rays and the trace's cannot come to
        // differ: this pass is handed the one struct, and the shader rebuilds the rays with the
        // trace's own `rayAt`.
        Shaders::AtrousConstants level{
            .mCamera = camera,
            .mStep = 1,
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

            // Sampled from `GENERAL` on the four this pass only reads. A `SAMPLED_IMAGE`
            // descriptor names the image alone and no sampler, which is what `sBindings` declares.
            DescriptorWrites<Shaders::ATROUS_BINDINGS> writes;
            writes.image(
                Shaders::ATROUS_BIND_SOURCE, source->describeSampled(VK_NULL_HANDLE), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            writes.image(Shaders::ATROUS_BIND_FILTERED, target->describeStorage());
            writes.image(Shaders::ATROUS_BIND_GUIDE, buffer.get(Channel::Guide).describeSampled(VK_NULL_HANDLE),
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            writes.image(Shaders::ATROUS_BIND_DEPTH, buffer.get(Channel::Depth).describeSampled(VK_NULL_HANDLE),
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            writes.image(Shaders::ATROUS_BIND_MOMENTS, moments.describeSampled(VK_NULL_HANDLE),
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);

            level.mStep = 1u << pass;

            dispatch(commands, mPipeline, writes.get(), level, groupsFor(camera.mWidth, Shaders::ATROUS_WORKGROUP),
                groupsFor(camera.mHeight, Shaders::ATROUS_WORKGROUP));

            // The next level reads what this one wrote, and writes whichever of the other two it is
            // not reading — the blend after the first level, and the scratch and the blend by turns
            // after that.
            source = target;
            target = pass == 0 ? &blended : (source == &blended ? &scratch : &blended);
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

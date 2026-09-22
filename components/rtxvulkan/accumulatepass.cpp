#include "accumulatepass.hpp"

#include <array>
#include <cassert>

#include <components/rtx/frameimage.hpp>
#include <components/rtx/shaders/accumulate.h>
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
        /// The channel being blended, the three the frame describes it with, the three a history
        /// arrives in, the two of those this pass writes back, and the blend the cascade reads.
        /// Ten and not eleven, because the first wavelet level writes the history this reads next
        /// frame — SVGF's feedback.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::ACCUMULATE_BINDINGS> sBindings
            = computeBindings<Shaders::ACCUMULATE_BINDINGS>(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    }

    AccumulatePass::AccumulatePass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mPipeline(device, sBindings, sizeof(Shaders::AccumulateConstants), {},
            shaderDirectory / "accumulate.comp.spv", "accumulate")
    {
    }

    const Image& AccumulatePass::record(VkCommandBuffer commands, AccumulateHistory& history, const GBuffer& buffer,
        const Shaders::Camera& camera, float far, bool reset) const
    {
        assert(far > 0.0f && "a frame with no far plane to scale a stored distance by");
        assert(history.getWidth() >= camera.mWidth && history.getHeight() >= camera.mHeight);

        const AccumulateHistory::Turn turn = history.turn();

        // Every image this frame writes is written whole before it is read, so each is discarded;
        // the last frame's accesses to all of them — the cascade's writes over the blend among them —
        // are behind the head barrier `CommandPool::begin` recorded. The first frame after a resize
        // has nothing behind it, and an image whose contents were never written is not zero — it is
        // whatever the allocation held, in no layout at all. Discarding the history too is what
        // makes the reset below a statement about the history rather than about the memory; on
        // every frame after, it rests where the last frame's writes left it.
        Barriers barriers(commands);

        if (turn.mFresh)
            for (const Image* image : { &turn.mColourBefore, &turn.mSurfaceBefore, &turn.mMomentsBefore })
                barriers.add(image->describeTransition(Use::sUndefined, Use::sComputeRead));

        for (const Image* image : { &turn.mColour, &turn.mSurface, &turn.mMoments, &turn.mBlended })
            barriers.add(image->describeTransition(Use::sUndefined, Use::sComputeWrite));

        barriers.flush();

        DescriptorWrites<Shaders::ACCUMULATE_BINDINGS> writes;
        writes.image(Shaders::ACCUMULATE_BIND_INDIRECT, buffer.get(Channel::Indirect).describeStorage());
        writes.image(Shaders::ACCUMULATE_BIND_MOTION, buffer.get(Channel::Motion).describeStorage());
        writes.image(Shaders::ACCUMULATE_BIND_GUIDE, buffer.get(Channel::Guide).describeStorage());
        writes.image(Shaders::ACCUMULATE_BIND_DEPTH, buffer.get(Channel::Depth).describeStorage());
        writes.image(Shaders::ACCUMULATE_BIND_HISTORY_COLOUR, turn.mColourBefore.describeStorage());
        writes.image(Shaders::ACCUMULATE_BIND_HISTORY_SURFACE, turn.mSurfaceBefore.describeStorage());
        writes.image(Shaders::ACCUMULATE_BIND_HISTORY_MOMENTS, turn.mMomentsBefore.describeStorage());
        writes.image(Shaders::ACCUMULATE_BIND_SURFACE_OUT, turn.mSurface.describeStorage());
        writes.image(Shaders::ACCUMULATE_BIND_MOMENTS_OUT, turn.mMoments.describeStorage());
        writes.image(Shaders::ACCUMULATE_BIND_BLENDED_OUT, turn.mBlended.describeStorage());
        assert(writes.size() == Shaders::ACCUMULATE_BINDINGS && "a binding the layout declares was left unwritten");

        const Shaders::AccumulateConstants constants{
            .mCamera = camera,
            .mReset = (reset || turn.mFresh) ? 1u : 0u,
            .mDistanceScale = Shaders::ACCUMULATE_DISTANCE_RANGE / far,
        };

        dispatch(commands, mPipeline, writes.get(), constants, groupsFor(camera.mWidth, Shaders::ACCUMULATE_WORKGROUP),
            groupsFor(camera.mHeight, Shaders::ACCUMULATE_WORKGROUP));

        return turn.mMoments;
    }
}

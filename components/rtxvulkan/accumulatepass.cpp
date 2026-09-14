#include "accumulatepass.hpp"

#include <array>
#include <cassert>
#include <cstddef>

#include <components/rtx/shaders/look.h>

#include "barriers.hpp"
#include "dispatch.hpp"
#include "gbuffer.hpp"

namespace Rtx
{
    namespace
    {
        /// The channel being blended, the four the frame describes it with, the three a history
        /// arrives in, the two of those this pass writes back, and the blend the cascade reads.
        /// Eleven and not twelve, because the first wavelet level writes the history this reads
        /// next frame — SVGF's feedback.
        constexpr std::size_t sBindingCount = 11;

        constexpr std::array<VkDescriptorSetLayoutBinding, sBindingCount> sBindings
            = computeBindings<sBindingCount>(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

        /// `SAMPLED` beside `STORAGE` on what the cascade after this reads. `AtrousPass` takes
        /// its taps through the texture unit and a sampled descriptor needs the bit at creation,
        /// which is a promise made here and kept there.
        constexpr VkImageUsageFlags sReadAndWrite = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    }

    AccumulatePass::AccumulatePass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mDevice(device)
        , mPipeline(device, sBindings, sizeof(Shaders::AccumulateConstants), {},
              shaderDirectory / "accumulate.comp.spv", "accumulate")
    {
    }

    void AccumulatePass::resize(std::uint32_t width, std::uint32_t height)
    {
        if (!mBlended.isEmpty() && mBlended.getWidth() == width && mBlended.getHeight() == height)
            return;

        for (std::size_t i = 0; i < 2; ++i)
        {
            mColour[i] = Image(mDevice, width, height, ACCUMULATE_COLOUR, sReadAndWrite,
                i == 0 ? "accumulate-colour-0" : "accumulate-colour-1");
            mSurface[i] = Image(mDevice, width, height, ACCUMULATE_SURFACE, VK_IMAGE_USAGE_STORAGE_BIT,
                i == 0 ? "accumulate-surface-0" : "accumulate-surface-1");
            mMoments[i] = Image(mDevice, width, height, ACCUMULATE_MOMENTS, sReadAndWrite,
                i == 0 ? "accumulate-moments-0" : "accumulate-moments-1");
        }

        // `TRANSFER_SRC` for `FrameImage::Accumulated`, which is the one figure `shot --tail` counts a
        // firefly in and the only image in the frame that holds a clamped bounce.
        mBlended = Image(mDevice, width, height, ATROUS_CHANNEL, sReadAndWrite | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            "accumulate-blended");

        mCurrent = 0;
        mFresh = true;
    }

    const Image& AccumulatePass::record(
        VkCommandBuffer commands, const GBuffer& buffer, const Shaders::Camera& camera, float far, bool reset)
    {
        assert(!mColour[0].isEmpty() && "record before resize");
        assert(far > 0.0f && "a frame with no far plane to scale a stored distance by");
        assert(mColour[0].getWidth() >= camera.mWidth && mColour[0].getHeight() >= camera.mHeight);

        const std::size_t previous = mCurrent;
        mCurrent = 1 - mCurrent;

        // The first frame after a resize has nothing behind it, and an image whose contents were
        // never written is not zero — it is whatever the allocation held. Discarding it is what makes
        // the reset below a statement about the history rather than about the memory.
        Barriers barriers(commands);

        const VkImageLayout held = mFresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        for (const Image* image : { &mColour[previous], &mSurface[previous], &mMoments[previous] })
            barriers.add(image->describeTransition(
                ImageUse{ held, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
                Use::sComputeRead));

        for (const Image* image : { &mColour[mCurrent], &mSurface[mCurrent], &mMoments[mCurrent] })
            barriers.add(image->describeTransition(
                ImageUse{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT },
                Use::sComputeWrite));

        // Waiting on both of the cascade's accesses and not only its read. Two frames are in
        // flight over one blend image, and the levels of the cascade write it as well as read it —
        // so a frame arriving here has to wait for the previous frame's odd levels to finish
        // writing, which a dependency naming the read alone would not order.
        barriers.add(
            mBlended.describeTransition(ImageUse{ VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                                | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT },
                Use::sComputeWrite));

        barriers.flush();

        DescriptorWrites<sBindingCount> writes;
        writes.image(0, buffer.get(Channel::Indirect).describeStorage());
        writes.image(1, buffer.get(Channel::Motion).describeStorage());
        writes.image(2, buffer.get(Channel::Guide).describeStorage());
        writes.image(3, buffer.get(Channel::Depth).describeStorage());
        writes.image(4, buffer.get(Channel::BiasMask).describeStorage());
        writes.image(5, mColour[previous].describeStorage());
        writes.image(6, mSurface[previous].describeStorage());
        writes.image(7, mMoments[previous].describeStorage());
        writes.image(8, mSurface[mCurrent].describeStorage());
        writes.image(9, mMoments[mCurrent].describeStorage());
        writes.image(10, mBlended.describeStorage());
        assert(writes.size() == sBindingCount && "a binding the layout declares was left unwritten");

        const Shaders::AccumulateConstants constants{
            .mCamera = camera,
            .mReset = (reset || mFresh) ? 1u : 0u,
            .mDistanceScale = Shaders::ACCUMULATE_DISTANCE_RANGE / far,
        };

        dispatch(commands, mPipeline, writes.get(), constants, groupsFor(camera.mWidth, Shaders::ACCUMULATE_WORKGROUP),
            groupsFor(camera.mHeight, Shaders::ACCUMULATE_WORKGROUP));

        mFresh = false;

        return mMoments[mCurrent];
    }

    const Image& AccumulatePass::getBlended() const
    {
        assert(!mBlended.isEmpty() && "the blend asked for before a resize made one");
        assert(!mFresh && "the blend asked for before any frame was accumulated into it");

        return mBlended;
    }

    const Image& AccumulatePass::getHistory() const
    {
        assert(!mColour[mCurrent].isEmpty() && "the history asked for before a resize made one");
        assert(!mFresh && "the history asked for before the frame that hands it over");

        return mColour[mCurrent];
    }
}

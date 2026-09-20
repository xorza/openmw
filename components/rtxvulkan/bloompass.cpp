#include "bloompass.hpp"

#include <array>
#include <cassert>
#include <format>

#include <osg/Vec2f>

#include <components/rtx/shaders/bloom.h>
#include <components/rtx/shaders/look.h>

#include "barriers.hpp"
#include "device.hpp"
#include "dispatch.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    namespace
    {
        /// What is being read, and what is being written. The first is sampled rather than loaded,
        /// because both kernels are counted in bilinear fetches.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sBindings{
            computeBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        };
    }

    BloomPass::BloomPass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mDevice(device)
        , mHalvePipeline(device, sBindings, sizeof(Shaders::BloomConstants), {}, shaderDirectory / "bloomdown.comp.spv",
              "bloom halve")
        , mSpreadPipeline(device, sBindings, sizeof(Shaders::BloomConstants), {}, shaderDirectory / "bloomup.comp.spv",
              "bloom spread")
        , mSampler(makeTargetSampler(device, "bloom"))
    {
    }

    void BloomPass::resize(std::uint32_t width, std::uint32_t height)
    {
        if (!mLevels.empty() && mLevels.front().getWidth() == width / 2 && mLevels.front().getHeight() == height / 2)
            return;

        mLevels.clear();

        for (std::uint32_t level = 0; level < Shaders::BLOOM_LEVELS; ++level)
        {
            width /= 2;
            height /= 2;

            if (width < Shaders::BLOOM_NARROWEST || height < Shaders::BLOOM_NARROWEST)
                break;

            // `TRANSFER_SRC` because the levels are the whole of what this pass produces and so the
            // only thing a reader can check it by — `GBuffer::sReadable` carries the bit for the
            // same reason, and it costs no memory either.
            mLevels.emplace_back(mDevice, width, height, BLOOM_LEVEL,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                std::format("bloom level {}", level));
        }
    }

    void BloomPass::handOver(VkCommandBuffer commands, const Image& level) const
    {
        level.transition(commands, Use::sComputeWrite, Use::sComputeSample);
    }

    void BloomPass::run(VkCommandBuffer commands, const ComputePipeline& pipeline, const Image& source,
        const Image& target, float mix) const
    {
        // Sampled from `GENERAL` rather than moved to a read-only layout. A level is written as
        // a storage image and read as a sampled one within a few dispatches of each other, and the
        // layout this renderer keeps everything in is one both accesses are legal from.
        DescriptorWrites<2> writes;
        writes.image(0, source.describeSampled(mSampler.get()), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        writes.image(1, target.describeStorage());

        const Shaders::BloomConstants constants{
            .mWidth = target.getWidth(),
            .mHeight = target.getHeight(),
            .mTexel
            = osg::Vec2f(1.0f / static_cast<float>(source.getWidth()), 1.0f / static_cast<float>(source.getHeight())),
            .mMix = mix,
        };

        dispatch(commands, pipeline, writes.get(), constants, groupsFor(target.getWidth(), Shaders::BLOOM_WORKGROUP),
            groupsFor(target.getHeight(), Shaders::BLOOM_WORKGROUP));
    }

    void BloomPass::record(VkCommandBuffer commands, const Image& frame) const
    {
        assert((frame.getUsage() & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 && "the pyramid samples the frame");

        // A frame too small for even one level has no pyramid, which `getPyramid` says and the
        // display pass reads as no lens at all. Every dispatch below would be over no pixels.
        if (mLevels.empty())
            return;

        assert(mLevels.front().getWidth() == frame.getWidth() / 2 && "record before resize");

        // Nothing has written the levels yet this frame, so the halvings may discard whatever the
        // last one left; the curve that sampled it is behind the head barrier `CommandPool::begin`
        // recorded.
        Barriers opened(commands);
        for (const Image& level : mLevels)
            opened.add(level.describeTransition(Use::sUndefined, Use::sComputeWrite));

        opened.flush();

        const Image* source = &frame;
        for (const Image& level : mLevels)
        {
            run(commands, mHalvePipeline, *source, level, 0.0f);
            handOver(commands, level);
            source = &level;
        }

        // Back up the pyramid, each level mixed into the one above it. The coarsest has nothing
        // coarser to take, which is why this starts one below the end. The finer level is about to
        // be read as well as written, and what it holds is its own halving from the loop above — a
        // write after a read after a write, all in one stage. The level the last spread wrote is
        // handed over in the same command, so the queue drains once between two spreads and not
        // twice.
        const Image* written = nullptr;
        for (std::size_t level = mLevels.size() - 1; level > 0; --level)
        {
            const Image& finer = mLevels[level - 1];

            Barriers between(commands);
            between.add(finer.describeTransition(Use::sComputeWrite, Use::sComputeReadWrite));
            if (written != nullptr)
                between.add(written->describeTransition(Use::sComputeWrite, Use::sComputeSample));
            between.flush();

            run(commands, mSpreadPipeline, mLevels[level], finer, Shaders::BLOOM_SCATTER);
            written = &finer;
        }

        // What the curve samples: the finest level, which a pyramid of one level handed over above.
        if (written != nullptr)
            handOver(commands, *written);
    }
}

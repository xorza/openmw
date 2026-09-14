#include "exposurepass.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "barriers.hpp"
#include "dispatch.hpp"
#include "image.hpp"

namespace Rtx
{
    namespace
    {
        /// The frame in, the histogram out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sHistogramBindings{
            computeBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        };

        /// The histogram in, the one float out.
        constexpr std::array<VkDescriptorSetLayoutBinding, 2> sReduceBindings
            = computeBindings<2>(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    }

    ExposurePass::ExposurePass(const Device& device, const std::filesystem::path& shaderDirectory)
        : mHistogramPipeline(device, sHistogramBindings, sizeof(Shaders::HistogramConstants), {},
            shaderDirectory / "histogram.comp.spv", "histogram")
        , mReducePipeline(device, sReduceBindings, sizeof(Shaders::ExposureConstants), {},
              shaderDirectory / "exposure.comp.spv", "exposure")
        , mHistogram(Buffer::deviceLocal(device, Shaders::EXPOSURE_BINS * sizeof(std::uint32_t),
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "histogram"))
        , mExposure(Buffer::deviceLocal(
              device, sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, "exposure"))
        , mPicture(Buffer::hostWritten(device, sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "picture exposure"))
    {
        mPicture.writable<float>(0, 1).front() = 1.0f;
    }

    void ExposurePass::recordFixed(VkCommandBuffer commands, float value) const
    {
        // Four bytes, so an inline write into the command buffer rather than a staging copy —
        // ordered against the curve still reading the previous frame's exposure, because two
        // frames in flight share the one buffer, and against the reduction that reads it as well
        // as writes it.
        mExposure.updateInline(commands, Use::sBufferComputeReadWrite, std::as_bytes(std::span(&value, 1)));
    }

    void ExposurePass::record(
        VkCommandBuffer commands, const Image& frame, float elapsedSeconds, bool reset, float bias) const
    {
        // Against the previous frame and not this one: two frames in flight share one set of these
        // buffers, so the measurement about to overwrite them may start while the curve reading
        // them is still running. An execution dependency is all a write-after-read needs — and
        // the exposure is read as well as written, because the reduction moves the previous
        // frame's exposure toward this frame's measurement, so the write before it has to be
        // visible and not merely ordered.
        constexpr BufferUse touched{ VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                | VK_ACCESS_2_TRANSFER_WRITE_BIT };

        Barriers before(commands);
        before.add(mHistogram.describeBarrier(touched, Use::sBufferClearWrite));
        before.add(mExposure.describeBarrier(touched, Use::sBufferComputeReadWrite));
        before.flush();

        // Cleared here and not in a shader: the workgroups accumulate into it, so one of them
        // zeroing it would race with the rest.
        mHistogram.clear(commands);
        mHistogram.transition(commands, Use::sBufferClearWrite, Use::sBufferComputeReadWrite);

        DescriptorWrites<2> binning;
        binning.image(0, frame.describeStorage());
        binning.buffer(1, mHistogram.describe());

        const Shaders::HistogramConstants extent{
            .mWidth = frame.getWidth(),
            .mHeight = frame.getHeight(),
        };

        dispatch(commands, mHistogramPipeline, binning.get(), extent,
            groupsFor(extent.mWidth, Shaders::HISTOGRAM_WORKGROUP),
            groupsFor(extent.mHeight, Shaders::HISTOGRAM_WORKGROUP));

        // The reduction has to see every pixel's contribution before it divides by the total, which
        // is what this dispatch boundary is for.
        mHistogram.transition(commands, Use::sBufferComputeWrite, Use::sBufferComputeRead);

        DescriptorWrites<2> reducing;
        reducing.buffer(0, mHistogram.describe());
        reducing.buffer(1, mExposure.describe());

        const Shaders::ExposureConstants counted{
            .mPixels = frame.getWidth() * frame.getHeight(),
            .mElapsed = elapsedSeconds,
            .mReset = reset ? 1u : 0u,
            .mBias = bias,
        };

        // One group, because the reduction is over the bins and the bins are one workgroup's worth.
        dispatch(commands, mReducePipeline, reducing.get(), counted, 1);

        // The curve reads what the reduction wrote.
        mExposure.transition(commands, Use::sBufferComputeWrite, Use::sBufferComputeRead);
    }
}

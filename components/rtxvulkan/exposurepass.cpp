#include "exposurepass.hpp"

#include <array>
#include <cstdint>
#include <span>

#include <components/rtx/shaders/exposure.h>

#include "dispatch.hpp"
#include "image.hpp"
#include "imageuse.hpp"

namespace Rtx
{
    namespace
    {
        /// The frame in, the histogram out.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::HISTOGRAM_BINDINGS> sHistogramBindings{
            computeBinding(Shaders::HISTOGRAM_BIND_SOURCE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            computeBinding(Shaders::HISTOGRAM_BIND_BINS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        };

        /// The histogram in, the one float out.
        constexpr std::array<VkDescriptorSetLayoutBinding, Shaders::EXPOSURE_BINDINGS> sReduceBindings
            = computeBindings<Shaders::EXPOSURE_BINDINGS>(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

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
        // Two frames in flight share one set of these buffers, and the previous frame's curve
        // reading them, its reduction writing the exposure this one moves toward, and its clear are
        // all behind the head barrier `CommandPool::begin` recorded — which is why the clear waits
        // for nothing of its own.
        //
        // Cleared here and not in a shader: the workgroups accumulate into it, so one of them
        // zeroing it would race with the rest.
        mHistogram.clear(commands);
        mHistogram.transition(commands, Use::sBufferClearWrite, Use::sBufferComputeReadWrite);

        DescriptorWrites<Shaders::HISTOGRAM_BINDINGS> binning;
        binning.image(Shaders::HISTOGRAM_BIND_SOURCE, frame.describeStorage());
        binning.buffer(Shaders::HISTOGRAM_BIND_BINS, mHistogram.describe());

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

        DescriptorWrites<Shaders::EXPOSURE_BINDINGS> reducing;
        reducing.buffer(Shaders::EXPOSURE_BIND_HISTOGRAM, mHistogram.describe());
        reducing.buffer(Shaders::EXPOSURE_BIND_EXPOSURE, mExposure.describe());

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

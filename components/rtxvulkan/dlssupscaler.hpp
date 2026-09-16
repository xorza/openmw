#pragma once

#include <memory>

#include <vulkan/vulkan_core.h>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/upscale.hpp>

#include "dlss.hpp"
#include "image.hpp"
#include "upscaler.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;
    class DlssPass;

    /// `Upscaler` as DLSS Ray Reconstruction: the process's runtime, the feature built for the
    /// current pair of extents, and the image it writes. Absent without `-DOPENMW_RTX_DLSS=ON`,
    /// where `makeUpscaler` refuses instead.
    class DlssUpscaler final : public Upscaler
    {
    public:
        /// Starts the runtime. Throws `Unsupported` where NGX says this machine cannot run it,
        /// after letting the runtime go, so that a machine that gains a driver need not be restarted
        /// twice and a second attempt is not refused by the one-runtime-per-process rule.
        DlssUpscaler(const Device& device, VkInstance instance);
        ~DlssUpscaler() override;

        VkExtent2D renderSizeFor(VkExtent2D output, Upscale mode) const override;
        void resize(CommandPool& pool, VkExtent2D render, VkExtent2D output, const Upscaling& how) override;
        void release() override;
        const Image& getOutput() const override { return mOutput; }
        const Image& record(VkCommandBuffer commands, const UpscaleInputs& inputs) override;

    private:
        const Device& mDevice;
        Dlss mNgx;

        /// Built for one pair of resolutions and so rebuilt by every resize.
        std::unique_ptr<DlssPass> mPass;

        /// What it writes: the frame at the output extent, still in linear radiance.
        Image mOutput;
    };
}

#pragma once

#include <vulkan/vulkan_core.h>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/upscale.hpp>

#include "dlss.hpp"
#include "upscaler.hpp"

// NGX's own, forward-declared for the reason `dlss.hpp` gives.
struct NVSDK_NGX_Handle;

namespace Rtx
{
    class Image;

    /// DLSS Ray Reconstruction, built for one pair of resolutions. The parameter map it was built
    /// from is allocated per feature, has to outlive it, and is released after it.
    class DlssPass
    {
    public:
        /// Builds Ray Reconstruction to take `render` and produce `output`. Throws `Unsupported`
        /// where NGX will not build it.
        ///
        /// @param commands must be recording, and submitted and waited on before the first
        ///        evaluation: NGX uploads the network's weights here.
        DlssPass(const Dlss& ngx, VkCommandBuffer commands, VkExtent2D render, VkExtent2D output, Upscale upscale,
            Preset preset);
        ~DlssPass();

        DlssPass(const DlssPass&) = delete;
        DlssPass& operator=(const DlssPass&) = delete;

        /// Records one upscale into `output`, at the output extent. Every image must be in
        /// `VK_IMAGE_LAYOUT_GENERAL` and hold this frame. Throws `DeviceError` where NGX refuses
        /// the evaluation.
        void record(VkCommandBuffer commands, const UpscaleInputs& inputs, const Image& output) const;

    private:
        NVSDK_NGX_Handle* mHandle = nullptr;
        NVSDK_NGX_Parameter* mParameters = nullptr;
        VkExtent2D mRenderExtent{};
        VkExtent2D mOutputExtent{};
    };
}

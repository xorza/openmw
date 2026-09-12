#include "dlsspass.hpp"

#include <cassert>

// **First, and in a block of its own so clang-format keeps it there.** The DLSSD helper below
// reaches for `NVSDK_NGX_Create_ImageView_Resource_VK` and `NVSDK_NGX_VK_GBuffer` without including
// the header that declares them, and sorted alphabetically it would come first.
#include <nvsdk_ngx_helpers_vk.h>

#include <nvsdk_ngx_helpers_dlssd_vk.h>
#include <nvsdk_ngx_vk.h>

#include <components/rtx/error.hpp>

#include "image.hpp"
#include "ngx.hpp"

namespace Rtx
{
    namespace
    {
        /// How the frame is described to Ray Reconstruction at creation. `IsHDR` because the trace
        /// writes scene-referred radiance and the tone curve comes after the upscale. `MVLowRes` is
        /// a description, not a request: the motion vectors *are* at the render resolution, and
        /// leaving it out is `FAIL_InvalidParameter` with the reason only in NGX's own log.
        /// `DepthInverted` is absent because this renderer's clip depth is zero at the near plane;
        /// `AutoExposure` and the exposure parameters are absent because Ray Reconstruction does
        /// not support exposure (integration guide §3.7), measured bit-identical with and without.
        constexpr int sCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

        /// An image as NGX takes one, checked against the size the feature was built for, because
        /// a guide at another resolution goes to the network unremarked: NGX returns success, the
        /// layers say nothing, and the picture is wrong. Read-write is a statement about the image
        /// and not about this call — `nvsdk_ngx_defs_vk.h` defines it as the `VkImage` carrying
        /// `VK_IMAGE_USAGE_STORAGE_BIT`, which every image here does.
        NVSDK_NGX_Resource_VK resourceOf(const Image& image, VkExtent2D expected)
        {
            assert((image.getUsage() & VK_IMAGE_USAGE_SAMPLED_BIT) != 0
                && "DLSS samples its inputs; one it cannot sample reads as zero and nothing reports it");
            assert(image.getWidth() == expected.width && image.getHeight() == expected.height
                && "an input at another resolution than the feature was built for, which NGX accepts in silence");

            const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            return NVSDK_NGX_Create_ImageView_Resource_VK(image.getView(), image.getHandle(), whole, image.getFormat(),
                image.getWidth(), image.getHeight(), true);
        }
    }

    DlssPass::DlssPass(
        const Dlss& ngx, VkCommandBuffer commands, VkExtent2D render, VkExtent2D output, Upscale upscale, Preset preset)
        : mRenderExtent(render)
        , mOutputExtent(output)
    {
        const NVSDK_NGX_Result allocated = NVSDK_NGX_VULKAN_AllocateParameters(&mParameters);
        if (NVSDK_NGX_FAILED(allocated) || mParameters == nullptr)
            throw Unsupported("NGX would not allocate a parameter map: " + describeNgxResult(allocated));

        // NGX caches a released feature's memory rather than freeing it, and a renderer that
        // follows a window through a drag creates a different feature every time — gigabytes over a
        // drag, until `vkAllocateMemory` refuses. The programming guide names this hint.
        NVSDK_NGX_Parameter_SetI(mParameters, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);

        // Set on the map before the feature is built, because it is read while it is built. Left
        // unset, the installed library picks, and what it picks has changed between SDK versions,
        // so two runs on two machines would not be the same measurement.
        NVSDK_NGX_Parameter_SetUI(
            mParameters, ngxPresetParameterOf(upscale), static_cast<unsigned int>(ngxPresetOf(preset)));

        NVSDK_NGX_DLSSD_Create_Params create{};
        create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        // Roughness comes from the normal target's fourth channel, so there is no separate resource
        // to bind and none to write.
        create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
        // **The enum is about the depth's shape, not where it came from.** `Linear` is 0 and `HW`
        // is 1, and what the trace writes is a projected clip value whichever shader computed it.
        // Saying `Linear` because a compute shader wrote it is true and irrelevant, and is the
        // second thing `FAIL_InvalidParameter` has meant here.
        create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
        create.InWidth = render.width;
        create.InHeight = render.height;
        create.InTargetWidth = output.width;
        create.InTargetHeight = output.height;
        create.InPerfQualityValue = ngxQualityOf(upscale);
        create.InFeatureCreateFlags = sCreateFlags;
        create.InEnableOutputSubrects = false;

        // One GPU, so both node masks are the first node.
        const NVSDK_NGX_Result built
            = NGX_VULKAN_CREATE_DLSSD_EXT1(ngx.getDevice(), commands, 1, 1, &mHandle, mParameters, &create);
        if (NVSDK_NGX_FAILED(built))
        {
            // A constructor that throws gets no destructor, and the map is already NGX's to free.
            NVSDK_NGX_VULKAN_DestroyParameters(mParameters);
            mParameters = nullptr;
            throw Unsupported("NGX would not build Ray Reconstruction: " + describeNgxResult(built));
        }
    }

    DlssPass::~DlssPass()
    {
        // The feature first, then the map it was built from: the map has to outlive it.
        if (mHandle != nullptr)
            NVSDK_NGX_VULKAN_ReleaseFeature(mHandle);
        if (mParameters != nullptr)
            NVSDK_NGX_VULKAN_DestroyParameters(mParameters);
    }

    void DlssPass::record(VkCommandBuffer commands, const DlssInputs& inputs) const
    {
        // Held by value across the call: the parameter map keeps the pointers rather than what they
        // point at, so every one of these has to outlive the evaluation. Each is checked against the
        // extent it belongs to as it is made — `resourceOf` says why there rather than here.
        NVSDK_NGX_Resource_VK colour = resourceOf(inputs.mColour, mRenderExtent);
        NVSDK_NGX_Resource_VK diffuse = resourceOf(inputs.mDiffuseAlbedo, mRenderExtent);
        NVSDK_NGX_Resource_VK specular = resourceOf(inputs.mSpecularAlbedo, mRenderExtent);
        NVSDK_NGX_Resource_VK normals = resourceOf(inputs.mNormalRoughness, mRenderExtent);
        NVSDK_NGX_Resource_VK depth = resourceOf(inputs.mDepth, mRenderExtent);
        NVSDK_NGX_Resource_VK motion = resourceOf(inputs.mMotion, mRenderExtent);
        NVSDK_NGX_Resource_VK target = resourceOf(inputs.mOutput, mOutputExtent);
        NVSDK_NGX_Resource_VK reflections = resourceOf(inputs.mReflectionMotion, mRenderExtent);
        NVSDK_NGX_Resource_VK particles = resourceOf(inputs.mParticleMask, mRenderExtent);
        NVSDK_NGX_Resource_VK bias = resourceOf(inputs.mBiasMask, mRenderExtent);
        NVSDK_NGX_Resource_VK layer = resourceOf(inputs.mTransparency, mRenderExtent);
        NVSDK_NGX_Resource_VK layerOpacity = resourceOf(inputs.mTransparencyOpacity, mRenderExtent);
        NVSDK_NGX_Resource_VK layerMotion = resourceOf(inputs.mTransparencyMotion, mRenderExtent);

        NVSDK_NGX_VK_DLSSD_Eval_Params evaluate{};
        evaluate.pInColor = &colour;
        evaluate.pInOutput = &target;
        evaluate.pInDepth = &depth;
        evaluate.pInMotionVectors = &motion;
        evaluate.pInDiffuseAlbedo = &diffuse;
        evaluate.pInSpecularAlbedo = &specular;
        evaluate.pInNormals = &normals;

        // What one motion vector per pixel cannot describe: where a reflection went, which pixels
        // are "not drawn as part of base pass", and which must not be accumulated across frames.
        // All three measured neutral or better on a lamp's convergence.
        evaluate.pInMotionVectorsReflections = &reflections;
        evaluate.pInIsParticleMask = &particles;
        evaluate.pInBiasCurrentColorMask = &bias;

        // The layer itself, and not the colour pair below it, which selects a path through the
        // network and measured worse.
        evaluate.pInTransparencyLayer = &layer;
        evaluate.pInTransparencyLayerOpacity = &layerOpacity;
        evaluate.pInTransparencyLayerMvecs = &layerMotion;

        // The four colour-pair guides, marked research in the header, are deliberately unset: both
        // pairs select a different path through the network rather than answer a question about the
        // frame — two identical images for the fog pair fail the same way. Measured on a turning
        // camera through Balmora as the vertical gradient down the edge bands: neither pair 0.537,
        // fog pair 0.339, sprite pair 1.385, both 1.385; and a lamp's peak byte over 1, 4, 16, 64
        // and 128 frames 83, 98, 111, 137, 149 with the fog pair against 101, 137, 161, 177, 179
        // without.

        // Negated, on both axes: the trace adds the offset to the sample coordinate, where NGX
        // wants it as applied to the projection. The wrong sign doubles the jitter instead of
        // cancelling it, and nothing reports it — the image just shakes by about a pixel a frame.
        evaluate.InJitterOffsetX = -inputs.mJitter.x();
        evaluate.InJitterOffsetY = -inputs.mJitter.y();

        // Already in pixels, so nothing needs scaling. The SDK's helper reads zero here as one,
        // which would work by accident; saying it is clearer.
        evaluate.InMVScaleX = 1.0f;
        evaluate.InMVScaleY = 1.0f;
        evaluate.InReset = inputs.mReset ? 1 : 0;
        evaluate.InFrameTimeDeltaInMsec = inputs.mFrameDeltaMs;
        evaluate.InRenderSubrectDimensions = NVSDK_NGX_Dimensions{ mRenderExtent.width, mRenderExtent.height };

        const NVSDK_NGX_Result ran = NGX_VULKAN_EVALUATE_DLSSD_EXT(commands, mHandle, mParameters, &evaluate);
        if (NVSDK_NGX_FAILED(ran))
            throw Error("Ray Reconstruction would not run: " + describeNgxResult(ran));
    }
}

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
        /// How the frame is described to Ray Reconstruction at creation.
        ///
        /// `IsHDR` because what the trace writes is scene-referred radiance rather than a tone-mapped
        /// image — the whole point of the G-buffer split, and the tone curve comes after the upscale.
        ///
        /// **`MVLowRes` reads as a description, not a request.** It says the motion vectors *are* at
        /// the low — render — resolution, which is where the trace writes them. Reasoning it the
        /// other way round and leaving it out is rejected with "Low resolution Motion Vectors
        /// required", a message that exists only in NGX's own log: the API returns
        /// `FAIL_InvalidParameter`, which names no parameter.
        ///
        /// **`DepthInverted` is deliberately absent**, unlike in the reference implementation: the
        /// clip depth this renderer writes is zero at the near plane and one at the far one.
        ///
        /// **`AutoExposure` is deliberately absent too**, and so are `DLSS.Pre.Exposure` and
        /// `DLSS.Exposure.Scale` beside it in the SDK's own helper: Ray Reconstruction does not
        /// support exposure at all, as its integration guide says in §3.7 and as the reference
        /// measured — bit-identical with them and without.
        constexpr int sCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

        /// An image as NGX takes one, checked against the size the feature was built for.
        ///
        /// **Every resource passes through here, which is why the check lives here.** `record` used
        /// to assert the colour and the output and none of the rest, so a guide, a depth, a motion
        /// field or a mask at another resolution went to the network unremarked — the same failure
        /// the `SAMPLED_BIT` assertion below exists for, where NGX returns success, the layers say
        /// nothing, and the picture is wrong. Asserting at the one place a resource is made means a
        /// channel added later cannot be the one nobody checked.
        ///
        /// **Read-write is a statement about the image and not about this call.** The SDK defines
        /// the flag as "true if the resource is available for read and write access… for VkImage
        /// resources: VkImageUsageFlags for the associated VkImage includes
        /// `VK_IMAGE_USAGE_STORAGE_BIT`" (`nvsdk_ngx_defs_vk.h`), and every image here is created
        /// with that bit. Passing `false` for the ones this evaluation only reads would be a false
        /// answer to the question actually asked.
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
            throw Error("NGX would not allocate a parameter map: " + describeNgxResult(allocated));

        // **Set on the map before the feature is built, because it is read while it is built.** The
        // create parameters carry no preset field; the hint is one of the values NGX picks up off
        // the map it is handed, and setting it afterwards would name a network for a feature that
        // already exists. Left unset, the installed library picks — and what it picks has changed
        // between SDK versions and between the convolutional and transformer models, so two runs on
        // two machines are not the same measurement.
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
            throw Error("NGX would not build Ray Reconstruction: " + describeNgxResult(built));
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

        NVSDK_NGX_VK_DLSSD_Eval_Params evaluate{};
        evaluate.pInColor = &colour;
        evaluate.pInOutput = &target;
        evaluate.pInDepth = &depth;
        evaluate.pInMotionVectors = &motion;
        evaluate.pInDiffuseAlbedo = &diffuse;
        evaluate.pInSpecularAlbedo = &specular;
        evaluate.pInNormals = &normals;

        // **What one motion vector per pixel cannot describe.** The vector is written from the
        // surface a primary ray hit, so what the frame reflects and what it composites in front of
        // that surface both need saying separately: where a reflection went, which pixels are
        // "not drawn as part of base pass", and which must not be accumulated across frames at all.
        //
        // All three sit in the block the header marks optional rather than the one it marks
        // research, and all three measured neutral or better on a lamp's convergence.
        evaluate.pInMotionVectorsReflections = &reflections;
        evaluate.pInIsParticleMask = &particles;
        evaluate.pInBiasCurrentColorMask = &bias;

        // **The four colour-pair guides are deliberately unset, and that is measured.** All of them
        // sit in the block the header marks `/*** OPTIONAL - only for research purposes ***/`, and
        // both pairs make the picture worse. The sprite pair nearly triples the horizontal smear
        // down a turning camera's edge bands; the fog pair stops a lamp's highlight converging at
        // all — over a hundred and twenty-eight frames of history it was still climbing, where with
        // the pair unset it settles by sixty-four.
        //
        // **Not their content.** Handing the fog pair two identical images — "the fog did nothing",
        // which cannot be wrong — fails the same way, and pointing the sprite pair's second
        // parameter at an image that is not `pInColor` reproduces its number to the last digit.
        // These select a different path through the network rather than answer a question about the
        // frame. The figures, on a turning camera through Balmora with sixteen frames of history,
        // as the vertical gradient down the edge bands: neither pair 0.537, fog pair alone 0.339,
        // sprite pair alone 1.385, both 1.385. And a lamp's peak byte over 1, 4, 16, 64 and 128
        // frames: 83, 98, 111, 137, 149 with the fog pair, still climbing, against 101, 137, 161,
        // 177, 179 without it, settled by sixty-four.

        // **Negated, on both axes.** The trace adds the offset to the *sample coordinate* — it moves
        // where inside its pixel a ray is fired — where NGX wants the offset as applied to the
        // projection, which moves the frustum the other way for the same picture. Handing over the
        // coordinate's sign leaves Ray Reconstruction un-jittering in the direction that doubles the
        // offset instead of cancelling it, and nothing reports it: the image still resolves, it just
        // shakes by about a pixel a frame.
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

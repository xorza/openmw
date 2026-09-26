#pragma once

#include <memory>
#include <span>
#include <string>

#include <osg/Vec2f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/reconstruction.hpp>
#include <components/rtx/upscale.hpp>

namespace Rtx
{
    class Device;
    class Image;

    /// Everything one reconstruction reads. Every image must have been created with
    /// `VK_IMAGE_USAGE_SAMPLED_BIT`, or it reads as zero with the library returning success and
    /// the layers silent; the pass asserts it.
    struct UpscaleInputs
    {
        /// The trace's radiance at render resolution, undenoised. Ray Reconstruction is the
        /// denoiser: handing it a filtered frame is asking it to reconstruct detail already blurred
        /// away.
        const Image& mColour;

        /// The surface's own albedo, with nothing of the path in it — this is divided out of the
        /// colour above, so anything folded into it comes back out of the light.
        const Image& mDiffuseAlbedo;

        /// Its reflectance at the angle it was seen from. Zero over anything shaded by a Lambert
        /// model, which is every solid surface this renderer has.
        const Image& mSpecularAlbedo;

        /// Shading normal in `xyz`, roughness in `w` — the feature is built for the packed layout,
        /// which is one resource fewer to write and to bind.
        const Image& mNormalRoughness;

        /// Clip depth, in the sense a rasterizer would have written it.
        const Image& mDepth;

        /// Where each surface stood on the previous frame's screen, less where it stands now, in
        /// render pixels.
        const Image& mMotion;

        /// Where what the water reflects stood on the previous frame's screen — see
        /// `GBuffer::get(Channel::ReflectionMotion)`.
        const Image& mReflectionMotion;

        /// Where inside its pixel this frame sampled, in render pixels — the same offset the trace
        /// was given.
        osg::Vec2f mJitter;

        /// How long since the previous frame, in milliseconds, or nought where there was none: a
        /// motion vector says how far something went and not how fast.
        float mFrameDeltaMs = 0.0f;

        /// Whether the previous frame is worth anything. True after a jump no motion vector can
        /// describe: a new cell, a teleport, the first frame after a resize.
        bool mReset = false;
    };

    /// What the frame asks of whatever reconstructs it across frames: what to trace at, and the
    /// frame at the output extent. Ray Reconstruction is the one behind it, and one runtime per
    /// process; a build without it has `makeUpscaler` refuse by name, and nothing else in the
    /// frame path knows which build it is.
    class Upscaler
    {
    public:
        virtual ~Upscaler() = default;

        Upscaler(const Upscaler&) = delete;
        Upscaler& operator=(const Upscaler&) = delete;

        /// What to trace at to produce `output` under `mode`, which must not be `Off`: the
        /// library's answer and not a ratio applied here, because a frame traced at anything else
        /// is a frame it will refuse. Throws `Unsupported` where it will not answer.
        virtual VkExtent2D renderSizeFor(VkExtent2D output, Upscale mode) const = 0;

        /// Builds the feature for one pair of extents and the image it writes, releasing the last:
        /// the feature holds the network's weights for one pair, and the image is sixteen bytes a
        /// pixel of the output, so neither is left behind for a pair that may not come back. Once
        /// per resolution, and it uploads the weights, so never per frame.
        virtual void resize(VkExtent2D render, VkExtent2D output, const Upscaling& how) = 0;

        /// Lets the feature and its image go and keeps the runtime, for a mode turned off that may
        /// not come back.
        virtual void release() = 0;

        /// The image `record` writes, named ahead of the trace: the set that carries what the puffs
        /// are composited over is pushed for every launch, before the reconstruction runs.
        virtual const Image& getOutput() const = 0;

        /// Records one reconstruction into `getOutput`, at the output extent and in
        /// `Use::sTraceReadWrite`, where the puffs want it. After `resize`.
        virtual void record(VkCommandBuffer commands, const UpscaleInputs& inputs) = 0;

    protected:
        Upscaler() = default;
    };

    /// Brings the runtime up, or throws `Unsupported` naming what is missing: the library, in a
    /// build without it, or what the library says this machine lacks.
    std::unique_ptr<Upscaler> makeUpscaler(const Device& device, VkInstance instance);

    /// One line for `info`: available, or why not — asked without standing a runtime up, because
    /// the runtime is one per process and its shutdown is unconditional.
    std::string describeUpscaling(const Device& device, VkInstance instance);

    /// What the runtime needs enabled on the instance and on the device, asked before either
    /// exists. Empty in a build without one. The device's list is what `Device` enables beside the
    /// required extensions, so `PhysicalDevice::profileOf` refuses a device that lacks any of it.
    std::span<const char* const> upscalerInstanceExtensions();
    std::span<const char* const> upscalerDeviceExtensions();
}

#pragma once

#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "image.hpp"

namespace Rtx
{
    class Device;

    /// The set a fog volume is addressed through, made once and outliving every volume made against
    /// it.
    ///
    /// **Separate from the volume for the reason `GBufferLayout` gives**: a pipeline layout names
    /// every set it will ever be handed, and `VisibilityPass` builds its pipelines before any camera
    /// has a size — so the layout has to exist before the images do.
    class FogVolumeLayout
    {
    public:
        explicit FogVolumeLayout(const Device& device);
        ~FogVolumeLayout();

        FogVolumeLayout(const FogVolumeLayout&) = delete;
        FogVolumeLayout& operator=(const FogVolumeLayout&) = delete;

        VkDescriptorSetLayout getHandle() const { return mHandle; }

    private:
        const Device& mDevice;
        VkDescriptorSetLayout mHandle = VK_NULL_HANDLE;
    };

    /// The air in front of the eye, integrated once for a block of pixels rather than once per pixel.
    ///
    /// **A frustum-aligned grid, and everything about it follows from the march it replaces.** That
    /// march walks `FOG_STEPS` steps down every primary ray, reads the coverage field at each of
    /// them and buys `FOG_SHADOW_RAYS` sun probes, for every pixel of every frame — and the field it
    /// integrates has no detail at a pixel's size. One column per `FOG_VOLUME_SCALE` squared pixels
    /// answers all of them, and the trace takes one trilinear fetch.
    ///
    /// **Two images, because the sun's phase function must stay at the pixel's resolution.** Mie
    /// scattering off eight-micrometre droplets throws a peak thousands of times isotropic within a
    /// degree of the sun's line, and a column is a quarter of a degree across — so a volume that
    /// baked the phase in would smear the blaze around a low sun over four times its width. The
    /// factor is the one term along a ray that depends on the direction and nothing else, so it
    /// divides out: `getSunward` holds the sun's transport with both the phase and the irradiance
    /// taken off it, and the trace puts them back per pixel.
    ///
    /// **The moons keep the column's phase and the sun does not.** Two more images would buy the
    /// same sharpness for two discs whose halos are a fraction of the sun's, in a frame where the
    /// sky term already dominates.
    class FogVolume
    {
    public:
        /// @param width, height the camera's, in pixels. The grid covers them at `FOG_VOLUME_SCALE`.
        FogVolume(const Device& device, const FogVolumeLayout& layout, std::uint32_t width, std::uint32_t height);
        ~FogVolume();

        FogVolume(const FogVolume&) = delete;
        FogVolume& operator=(const FogVolume&) = delete;

        /// How many columns across and down the grid is — **not pixels**, which is what the
        /// `GBuffer` beside it measures in.
        std::uint32_t getColumns() const { return mColumns; }
        std::uint32_t getRows() const { return mRows; }

        /// The set both passes bind: the two images sampled, then the two written.
        VkDescriptorSet getSet() const { return mSet; }

        /// Takes both images for writing, waiting on whatever read them for the frame before.
        ///
        /// From undefined, for the reason `GBuffer::begin` gives: every texel is written before any
        /// is read, and one volume serves every frame in flight.
        void begin(VkCommandBuffer commands) const;

        /// Orders the dispatch that wrote them against the trace that samples them.
        void handOver(VkCommandBuffer commands) const;

    private:
        void destroy();

        const Device& mDevice;

        std::uint32_t mColumns = 0;
        std::uint32_t mRows = 0;

        /// Everything but the sun, accumulated front to back: the sky's own colour, both moons and
        /// every lamp in `rgb`, and what is left of a ray at that depth in `a`.
        Image mAir;

        /// The sun's transport to that depth — what the shadow rays and the fog's own column left of
        /// it — with the irradiance and the phase function divided out. `a` carries nothing.
        Image mSunward;

        /// Linear on all three axes and clamped on all three: a column at the edge of the screen has
        /// no neighbour outside it, and the nearest and furthest slices are the whole of what a ray
        /// shorter or longer than the grid can be charged for.
        VkSampler mSampler = VK_NULL_HANDLE;

        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSet = VK_NULL_HANDLE;
    };
}

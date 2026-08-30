#pragma once

#include <array>
#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "image.hpp"

namespace Rtx
{
    class CommandPool;
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
    /// **Two volumes and not one, because what filters and what a pixel reads are different
    /// quantities.** A froxel's scattering and extinction are properties of the *point* — they
    /// reproject into the previous frame exactly, so a jittered sample can be averaged with what
    /// stood there before. What the trace wants is the integral along a ray from *this* eye, which
    /// reprojects into nothing at all: the eye moved, so the previous frame's integral to the same
    /// world point covered a different segment. So the point quantities are filtered and the
    /// integral is taken afterwards, from the filtered volume, every frame. That split is what every
    /// shipping froxel volumetric does, and skipping it is what put the grid on screen.
    ///
    /// **Three pairs, then.** The point pair ping-pongs, because a frame reads the one it is not
    /// writing. The integrated pair does not, because nothing reads it across a frame.
    ///
    /// **The sun keeps a channel of its own throughout, because its phase function must stay at the
    /// pixel's resolution.** Mie scattering off eight-micrometre droplets throws a peak thousands of
    /// times isotropic within a degree of the sun's line, and a column is a quarter of a degree
    /// across — so a volume that baked the phase in would smear the blaze around a low sun over four
    /// times its width. The factor is the one term along a ray that depends on the direction and
    /// nothing else, so it divides out: the sun channel holds its transport with both the phase and
    /// the irradiance taken off it, and the trace puts them back per pixel.
    ///
    /// **The moons keep the column's phase and the sun does not.** Two more channels would buy the
    /// same sharpness for two discs whose halos are a fraction of the sun's, in a frame where the
    /// sky term already dominates.
    class FogVolume
    {
    public:
        /// @param pool used once, to lay every image out. **A history has to exist before it is
        ///        read**, and the copy a first frame reprojects into was never written by anything:
        ///        without this it is still `VK_IMAGE_LAYOUT_UNDEFINED` when the first dispatch binds
        ///        it. Its contents are never read — a first frame carries no basis to reproject
        ///        with — so it is laid out and not cleared.
        /// @param width, height the camera's, in pixels. The grid covers them at `FOG_VOLUME_SCALE`.
        FogVolume(const Device& device, CommandPool& pool, const FogVolumeLayout& layout, std::uint32_t width,
            std::uint32_t height);
        ~FogVolume();

        FogVolume(const FogVolume&) = delete;
        FogVolume& operator=(const FogVolume&) = delete;

        /// How many columns across and down the grid is — **not pixels**, which is what the
        /// `GBuffer` beside it measures in.
        std::uint32_t getColumns() const { return mColumns; }
        std::uint32_t getRows() const { return mRows; }

        /// The set both passes bind for a frame of this parity: the point pair as it stood last
        /// frame, the same pair to write this frame, and the integrated pair.
        VkDescriptorSet getSet(std::uint64_t frame) const { return mSets[writtenAt(frame)]; }

        /// Takes every image for writing, waiting on whatever read them for the frame before.
        ///
        /// **The point pair is not discarded**, because the frame about to be drawn reads what the
        /// frame before left in it. Only the half being written this frame comes from undefined, and
        /// only the integrated pair does so unconditionally.
        void begin(VkCommandBuffer commands, std::uint64_t frame) const;

        /// Orders the dispatch that wrote them against the trace that samples them.
        void handOver(VkCommandBuffer commands) const;

    private:
        /// Which of the point pair a frame writes, the other being what it reads as history. The
        /// set at that index is the one wired that way round, so the two cannot drift apart.
        static std::size_t writtenAt(std::uint64_t frame) { return frame & 1; }

        void destroy();

        const Device& mDevice;

        std::uint32_t mColumns = 0;
        std::uint32_t mRows = 0;

        /// What the air scatters and takes out at a point: the sky's own colour, both moons and
        /// every lamp in `rgb`, and the extinction per world unit in `a`. **Not integrated** — this
        /// is the pair a frame reprojects and averages.
        std::array<Image, 2> mScatter;

        /// The sun's transport to that point — what the shadow rays and the fog's own column left of
        /// it — with the irradiance and the phase function divided out. `a` carries nothing.
        std::array<Image, 2> mSunward;

        /// The same two quantities accumulated front to back, which is what a pixel reads. `a` of
        /// the first is what is left of a ray at that depth.
        Image mAir;
        Image mAirSunward;

        /// Linear on all three axes and clamped on all three: a column at the edge of the screen has
        /// no neighbour outside it, and the nearest and furthest slices are the whole of what a ray
        /// shorter or longer than the grid can be charged for.
        VkSampler mSampler = VK_NULL_HANDLE;

        VkDescriptorPool mPool = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, 2> mSets{};
    };
}

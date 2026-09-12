#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "handles.hpp"
#include "image.hpp"
#include "owned.hpp"

namespace Rtx
{
    class CommandPool;
    class Device;

    /// The fog's fractal field, on the device. Drawn once for the run and never again: what the
    /// weather, the hour and the cell decide is the extinction, the layer's height and how much of
    /// the band is cut, every one a number the shader already has. `Rtx::bakeFogNoise` says what is
    /// in it.
    class FogTile
    {
    public:
        /// @param pool submits the one upload and waits for it. Not on the frame path.
        FogTile(const Device& device, CommandPool& pool);

        /// The shape a coverage band is cut out of, and a second field decorrelated from it.
        const Image& getField() const { return mField; }

        /// Linear, mipmapped and wrapping on all three axes — the field is laid down every tile, and
        /// a tap that clamped would smear the last texel of one across the whole landscape.
        VkSampler getSampler() const { return mSampler.get(); }

    private:
        Image mField;
        Sampler mSampler;
    };

    /// The air in front of the eye, integrated once for a block of pixels rather than once per pixel.
    ///
    /// A frustum-aligned grid replacing a march of `FOG_STEPS` steps and eight sun probes down
    /// every primary ray of every frame, over a field that has no detail at a pixel's size. One
    /// column per `FOG_VOLUME_SCALE` squared pixels answers all of them. A room's air is drawn here
    /// too: its even field integrates in closed form, but that form still needs a lamp reservoir
    /// and a shadow ray per pixel, which a froxel does once for a column.
    ///
    /// Two volumes and not one, because what filters and what a pixel reads are different
    /// quantities: a froxel's scattering and extinction are properties of the point and reproject
    /// into the previous frame exactly, while the integral along a ray from *this* eye reprojects
    /// into nothing. So the point quantities are filtered — a pair that ping-pongs — and the
    /// integral is taken afterwards from the filtered volume, every frame; skipping that split is
    /// what put the grid on screen. Every slice is a sample at its own middle and the air between
    /// two is the line between them (`FogSlice`), and `fogdepth.comp` keeps a froxel's samples in
    /// the air short of the surface.
    ///
    /// The sun keeps a channel of its own, because its phase function must stay at the pixel's
    /// resolution: Mie scattering throws a peak thousands of times isotropic within a degree of the
    /// sun's line, and a column is a quarter of a degree across. The factor depends on the direction
    /// alone, so it divides out and the trace puts it back per pixel. The moons keep the column's
    /// phase: two more channels would buy the same sharpness for halos a fraction of the sun's.
    class FogVolume
    {
    public:
        /// @param pool used once, to lay every image out and empty it: the copy a first frame
        ///        reprojects into was never written by anything, and emptied rather than merely laid
        ///        out because nothing times a not-a-number is still one.
        /// @param width, height the camera's, in pixels. The grid covers them at `FOG_VOLUME_SCALE`.
        FogVolume(const Device& device, CommandPool& pool, const SetLayout& layout, std::uint32_t width,
            std::uint32_t height);

        /// The set every fog volume is addressed through, made once and outliving all of them, for
        /// the reason `GBuffer::describeLayout` gives.
        static SetLayout describeLayout(const Device& device);

        /// How many columns across and down the grid is — **not pixels**, which is what the
        /// `GBuffer` beside it measures in.
        std::uint32_t getColumns() const { return mColumns; }
        std::uint32_t getRows() const { return mRows; }

        /// The set every pass binds for a frame of this parity: the point pair as it stood last
        /// frame, the same pair and the lamps to write this frame, and the integrated pair.
        VkDescriptorSet getSet(std::uint64_t frame) const { return mSets[writtenAt(frame)]; }

        /// Takes every image for what the frame ahead does to it, waiting on whatever read them for
        /// the frame before. The point pair's read half is not discarded; only what is written whole
        /// before it is read comes from undefined.
        void begin(VkCommandBuffer commands, std::uint64_t frame) const;

        /// Orders the pass that finds each column's surface against the pass that fills the froxels.
        void depthTaken(VkCommandBuffer commands) const;

        /// Orders the pass that fills the froxels against the pass that integrates the columns, and
        /// against the trace, which reads the seeings and the lamps at a point for a puff of smoke
        /// (`puffLight`).
        void scattered(VkCommandBuffer commands, std::uint64_t frame) const;

        /// Orders the dispatch that wrote the accumulation and the slices against the trace that
        /// samples them.
        void handOver(VkCommandBuffer commands) const;

    private:
        /// Which of the point pair a frame writes, the other being what it reads as history. The
        /// set at that index is the one wired that way round, so the two cannot drift apart.
        static std::size_t writtenAt(std::uint64_t frame) { return frame & 1; }

        std::uint32_t mColumns = 0;
        std::uint32_t mRows = 0;

        /// What the air scatters and takes out at a point: the sky's own colour, both moons and
        /// every lamp in `rgb`, and the extinction per world unit in `a`. **Not integrated** — this
        /// is the pair a frame reprojects and averages.
        std::array<Image, 2> mScatter;

        /// The sun's transport to that point, with the irradiance and the phase function divided
        /// out, in `r`; what the lamp ray from that froxel found in `g`; what the ambient's found in
        /// `b`. Three answers of one ray each, filtered together because each is nought or one at
        /// an edge the grid cannot resolve. The air reads the first two and a puff of smoke reads
        /// all three (`puffLight`). One channel for the sun, because its transport is a product of
        /// transmittances and carries no colour; `sunInAir` says why water is asked at a point.
        std::array<Image, 2> mSunward;

        /// What every lamp reaching a froxel delivers into it, per steradian, with nothing standing
        /// in the way — integrated over the froxel's stretch rather than sampled. One image and not
        /// a pair, because an integral carries no draw to average away and a flicker filtered with
        /// the rest would lag the lantern by a sixth of a second.
        Image mLamps;

        /// The same two quantities accumulated front to back, which is what a pixel reads. `a` of
        /// the first is what is left of a ray at that depth; the second is one channel, the sun's.
        Image mAir;
        Image mAirSunward;

        /// What each slice holds once every filter is applied — `FogSlice`, as two images — which
        /// a pixel steps through from the last edge it passed to where its surface stands. Written
        /// by the integrate pass, so a pixel does not apply the lamps and the tent a second way.
        Image mSlice;
        Image mSliceSunward;

        /// How far each column's ray runs this frame before it meets a surface, in world units —
        /// what keeps a froxel's samples in the air. One float a column, never sampled.
        Image mColumnDepth;

        /// What each moon puts into the air along each column's ray, one layer a moon. The phase
        /// function is one number for the whole ray, so the depth pass works it out once.
        Image mColumnMoons;

        /// Linear on all three axes and clamped on all three: a column at the edge of the screen has
        /// no neighbour outside it, and the nearest and furthest slices are the whole of what a ray
        /// shorter or longer than the grid can be charged for.
        Sampler mSampler;

        Owned<VkDescriptorPool, vkDestroyDescriptorPool> mPool;
        std::array<VkDescriptorSet, 2> mSets{};
    };
}

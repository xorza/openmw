#pragma once

#include <array>
#include <cstdint>

#include <vulkan/vulkan_core.h>

#include "descriptorsets.hpp"
#include "frameslots.hpp"
#include "handles.hpp"
#include "image.hpp"

namespace Rtx
{
    class Device;

    /// The fog's fractal field, on the device, drawn once for the run: what the weather, the hour
    /// and the cell decide are numbers the shader already has. `Rtx::bakeFogNoise` says what is in it.
    class FogTile
    {
    public:
        /// `pool` submits the one upload and waits for it. Not on the frame path.
        explicit FogTile(const Device& device);

        /// The shape a coverage band is cut out of, and a second field decorrelated from it.
        const Image& getField() const { return mField; }

        /// Linear, mipmapped and wrapping on all three axes: a tap that clamped would smear the last
        /// texel of one tile across the whole landscape.
        VkSampler getSampler() const { return mSampler.get(); }

    private:
        Image mField;
        Sampler mSampler;
    };

    /// The air in front of the eye, integrated once for a column of `FOG_VOLUME_SCALE` squared
    /// pixels rather than once per pixel, over a field that has no detail at a pixel's size. Two
    /// volumes, because a froxel's scattering and extinction are properties of the point and
    /// reproject into the previous frame exactly, while the integral along a ray from this eye
    /// reprojects into nothing: the point pair is filtered and the integral taken afterwards from
    /// the filtered volume, every frame. The sun keeps a channel of its own with its phase divided
    /// out, because Mie scattering peaks within a degree of the sun's line and a column is a
    /// quarter of a degree across; the trace puts the phase back per pixel.
    class FogVolume
    {
    public:
        /// `pool` is used once, to lay every image out and empty it, because nothing times a
        /// not-a-number is still one. `width` and `height` are the camera's, in pixels.
        FogVolume(const Device& device, const SetLayout& layout, std::uint32_t width, std::uint32_t height);

        /// The set every fog volume is addressed through, made once and outliving all of them, for
        /// the reason `GBuffer::describeLayout` gives.
        static SetLayout describeLayout(const Device& device);

        /// How many columns across and down the grid is — not pixels.
        std::uint32_t getColumns() const { return mColumns; }
        std::uint32_t getRows() const { return mRows; }

        /// The set every pass binds for a trace in this slot: the point pair as it stood at the
        /// last trace, the same pair and the lamps to write this one, and the integrated pair.
        VkDescriptorSet getSet(const FrameSlot trace) const { return mSets.get(trace.get()); }

        /// Takes every image for what the trace ahead does to it, waiting on whatever read them for
        /// the trace before. Only what is written whole before it is read comes from undefined.
        void begin(VkCommandBuffer commands, FrameSlot trace) const;

        /// Orders the pass that finds each column's surface against the pass that fills the froxels.
        void depthTaken(VkCommandBuffer commands) const;

        /// Orders the pass that fills the froxels against the pass that integrates the columns, and
        /// against the trace, which reads a point for a puff of smoke (`puffLight`).
        void scattered(VkCommandBuffer commands, FrameSlot trace) const;

        /// Orders the dispatch that wrote the accumulation and the slices against the trace.
        void handOver(VkCommandBuffer commands) const;

    private:
        /// The point pair, and so the sets: one wired each way round. Which of the pair a trace
        /// writes is its `FrameSlot`, the other being its history — the trace's own slot and not
        /// the sample index's parity, which the host advances on frames that trace nothing and a
        /// run restarts at every stop, so two traces in a row could land on one copy.
        static_assert(sFrameSlots == 2, "the point pair is one copy per trace in flight");
        static constexpr std::uint32_t sParities = sFrameSlots;

        std::uint32_t mColumns = 0;
        std::uint32_t mRows = 0;

        /// What the air scatters and takes out at a point: the sky, both moons and every lamp in
        /// `rgb`, the extinction per world unit in `a`. The pair a frame reprojects and averages.
        std::array<Image, sParities> mScatter;

        /// The sun's transport to that point with the phase divided out in `r`, what the lamp ray
        /// found in `g`, what the ambient's found in `b`: three answers of one ray each, filtered
        /// together because each is nought or one at an edge the grid cannot resolve.
        std::array<Image, sParities> mSunward;

        /// What every lamp reaching a froxel delivers into it, per steradian, integrated over the
        /// froxel's stretch. One image, because an integral carries no draw to average away and a
        /// filtered flicker would lag the lantern.
        Image mLamps;

        /// The same two quantities accumulated front to back, which is what a pixel reads. `a` of
        /// the first is what is left of a ray at that depth; the second is the sun's one channel.
        Image mAir;
        Image mAirSunward;

        /// What each slice holds once every filter is applied (`FogSlice`), which a pixel steps
        /// through from the last edge it passed to where its surface stands.
        Image mSlice;
        Image mSliceSunward;

        /// How far each column's ray runs this frame before it meets a surface, in world units.
        Image mColumnDepth;

        /// What each moon puts into the air along each column's ray, one layer a moon.
        Image mColumnMoons;

        /// Linear and clamped on all three axes: a column at the edge of the screen has no
        /// neighbour, and the nearest and furthest slices are all a shorter or longer ray can be
        /// charged for.
        Sampler mSampler;

        DescriptorSets mSets;
    };
}

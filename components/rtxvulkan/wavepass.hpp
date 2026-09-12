#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include <osg/Vec2f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/shaders/wave.h>
#include <components/rtx/wavecascade.hpp>
#include <components/rtx/wavespectrum.hpp>

#include "buffer.hpp"
#include "computepipeline.hpp"
#include "handles.hpp"
#include "image.hpp"

namespace Rtx
{
    class Graveyard;
    class CommandPool;
    class Device;

    /// The sea, synthesised into textures a trace reads: one tile a cascade, three textures a tile.
    /// A frame turns the phases and inverse-transforms them, and that is all it does. What comes
    /// out is a height field and its first two derivatives, because the normal is the gradient and
    /// the caustics are the curvature, with the second moments beside them: a mip level carries the
    /// mean of what it covers and the mean of its square, and the difference is the variance that
    /// was averaged away — LEAN mapping, for one fetch that was happening anyway.
    class WavePass
    {
    public:
        /// @param pool used by `describe`, which is not on the frame path. Held, because a sea state
        ///        can arrive at any time.
        WavePass(const Device& device, CommandPool& pool, const std::filesystem::path& shaderDirectory);

        /// Draws the amplitudes for a sea state, replacing whatever was drawn before. Submits and
        /// waits. Does nothing where the sea is the one already described.
        /// @param graveyard where the spectrum this replaces goes. A frame in flight may still be
        ///        synthesising from it, which is what a weather turning the wind makes happen.
        void describe(const SeaState& sea, Graveyard& graveyard);

        /// Turns the phases to `seconds` and rebuilds every texture and every level from them,
        /// leaving each in `VK_IMAGE_LAYOUT_GENERAL` ordered against a sampled read. A cell with no
        /// water never samples them, so it need not synthesise them.
        void record(VkCommandBuffer commands, float seconds) const;

        /// Linear, mipmapped and wrapping — a tile lays the same water down every `getExtent` units,
        /// and a tap that clamped would smear the last texel of one across the whole sea.
        VkSampler getSampler() const { return mSampler.get(); }

        /// The two slopes, their own second moment, and the elevation squared.
        const Image& getSurface(std::size_t cascade) const { return *mTiles[cascade].mSurface; }

        const Image& getCurvature(std::size_t cascade) const { return *mTiles[cascade].mCurvature; }

        /// How wide this tile is in world units, which is what turns a world position into a
        /// texture coordinate and a cone width into a level.
        float getExtent(std::size_t cascade) const { return sWaveTiles[cascade].mExtent; }

        /// How wide one texel of that tile is, which is what a cone width becomes a level against.
        float getTexel(std::size_t cascade) const
        {
            return sWaveTiles[cascade].mExtent / static_cast<float>(sWaveTiles[cascade].mGrid);
        }

        /// What `Rtx::waveSlope` made of the sea last described, held rather than fetched from the
        /// coarsest level at every step of a march.
        float getSlope() const { return mSlope; }

        /// What `Rtx::waveCurvature` made of the sea last described, held for the same reason.
        const WaveCurvature& getMoments() const { return mCurvature; }

    private:
        /// What one tile of the sea occupies.
        struct Tile
        {
            /// The complex amplitudes and how fast each turns, drawn by `describe` and device-local
            /// — a frame reads every one of them, and three megabytes fetched across the bus each
            /// time is what a host-visible table would cost.
            Buffer mAmplitudes;
            Buffer mFrequencies;

            /// The three packed spectra laid end to end, transformed in place.
            Buffer mField;

            std::unique_ptr<Image> mSurface;
            std::unique_ptr<Image> mCurvature;
        };

        /// Orders the dispatch just recorded against the one about to read what it wrote.
        void handOver(VkCommandBuffer commands) const;

        /// The inverse transform of a tile's three packed fields, each along its rows and then along
        /// its columns.
        void transform(VkCommandBuffer commands, const Tile& tile, std::uint32_t count) const;

        const Device& mDevice;
        CommandPool& mPool;

        ComputePipeline mFormPipeline;
        ComputePipeline mLinePipeline;
        ComputePipeline mComposePipeline;

        Sampler mSampler;

        std::array<Tile, Shaders::WAVE_CASCADES> mTiles;

        /// What `describe` last drew for, so a placement that changed nothing about the water
        /// redraws nothing.
        SeaState mSea;
        bool mDrawn = false;

        float mSlope = 0.0f;
        WaveCurvature mCurvature;
    };
}

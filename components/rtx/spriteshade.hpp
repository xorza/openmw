#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>

#include "shaders/scene.h"

namespace Rtx
{
    /// How many layers of its own emitter stand between each sprite and a light, counted each frame.
    ///
    /// **A column of smoke has a sunlit side and a shaded side, and nothing per sprite can give it
    /// one.** The bake shadows a puff by its own texture and the wrap gives it a side, but the eye
    /// composites twenty of them over a pixel and their coverage-weighted mean washes what each did
    /// into one flat colour. What a column needs is shadow *between* sprites: what the ones nearer
    /// the sun leave of it for the ones behind. That is a question about the emitter's sprites and
    /// the light alone, so it is answered here, once per frame on the host, and the shader reads one
    /// number per sprite per light.
    ///
    /// **Counted in layers and not in transmittance, because the host does not hold the texture.**
    /// A sprite that stands in the light's path to another counts for its own fade — one layer for a
    /// whole puff, a fraction for a wisp — and the shader thins the light by what one layer of that
    /// texture hides on average, which is the texture's coarsest level. `(1 - mean) ^ layers` is
    /// exact where every layer is whole, and for a faded one it is the approximation the bake
    /// already takes.
    ///
    /// **A grid across the light and not every pair**, so that a storm's thousands cost per sprite
    /// what a candle's dozen do. Sprites are walked from the light outward; each reads what has been
    /// laid down at its own point and then lays its own disc down on top. The grid is the emitter's
    /// reach, square, and a disc lands on it as an antialiased footprint: whole inside, a one-cell
    /// ramp at the rim — which keeps the disc's area to first order — and its area as a point where
    /// it is too small to reach a cell's centre.
    ///
    /// Only what covers and faces the eye is shaded. A flame emits and shadows nothing of its own
    /// kind, and a rain streak is a thin thing seen by what passes through it.
    class SpriteShade
    {
    public:
        /// Cells along each side of the grid.
        ///
        /// Thirty-two puts a chimney's column six cells wide, which is enough to give it two sides.
        static constexpr std::uint32_t sCells = 32;

        /// How many cells the largest sprite of an emitter may span in radius.
        ///
        /// **What bounds the cost of a sprite.** A grid fine enough for a column's wisps would put a
        /// candle's one big puff over the whole of it; coarsening the grid to the largest sprite
        /// keeps a footprint to a few hundred cells whatever the emitter is.
        static constexpr float sLargestInCells = 8.0f;

        /// Writes `mSunLayers` and `mSkyLayers` of every sprite.
        /// @param toSun unit, toward the sun. The sky is straight up.
        void shade(std::span<Shaders::GpuSprite> sprites, std::span<const Shaders::GpuEmitter> emitters,
            const osg::Vec3f& toSun);

    private:
        /// One sprite of the run, seen along the light: how far toward it, and where across.
        struct Projected
        {
            float mDepth;
            float mAcross;
            float mUpward;
        };

        /// One emitter's run against one light.
        void shadeToward(std::span<Shaders::GpuSprite> run, const Shaders::GpuEmitter& emitter,
            const osg::Vec3f& toward, float Shaders::GpuSprite::*into);

        /// What has been laid down at a point of the grid, between the four cells around it.
        float readAt(float x, float y) const;

        /// Lays a disc of `radius` cells down about a point of the grid, `weight` deep.
        void layDown(float x, float y, float radius, float weight);

        std::vector<float> mGrid;
        std::vector<Projected> mProjected;
        std::vector<std::uint32_t> mOrder;
    };
}

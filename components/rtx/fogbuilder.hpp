#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <osg/Vec2i>
#include <osg/Vec3f>

#include <components/misc/constants.hpp>

namespace Rtx
{
    /// The fog's fractal field, drawn once into a wrapping volume with a chain of levels under it:
    /// value noise off a hashed lattice costs eight hashes an octave at every step of a march at
    /// every pixel, so a sampler reads it instead. One octave, wrapping on all three axes; the
    /// fractal is the shader's, which reads this at three scales that never come back into step.
    struct FogNoise
    {
        /// Every level end to end, the full one first, two channels a texel, slice by slice.
        std::vector<std::uint8_t> mBytes;

        /// Where each level begins in `mBytes`.
        std::vector<std::size_t> mOffsets;
    };

    /// Draws the tile. Every level is normalised to one mean and one spread, so a coarse step
    /// loses the detail and never the amount of air.
    FogNoise bakeFogNoise();

    /// One exterior cell's side, in units, as a float once for every ring that measures by it.
    inline constexpr float sCellSize = static_cast<float>(Constants::CellSizeInUnits);

    /// The cell a world position stands in, on the exterior grid.
    osg::Vec2i cellOf(const osg::Vec3f& position);

    /// Whether `cell` is within `band` cells of `eye` on both axes — the square the rings are.
    bool withinCells(const osg::Vec2i& cell, const osg::Vec2i& eye, int band);

    /// How far from the eye the world is built, in units: one number for the ground and the air,
    /// because an extinction tuned to a shorter distance swallows everything past the active grid.
    /// `cells` is `[RTX] distant land cells`, and nought hands the decision back to
    /// `viewingDistance`, the rasterizer's knob.
    float distantLandReach(float cells, float viewingDistance);

    /// The air in a cell, in the units the shader takes.
    struct Fog
    {
        /// What the air scatters toward the eye, linear: out of doors `fogColour` of the dome's
        /// mean, and a room's record as it is.
        osg::Vec3f mColour;

        /// How fast it swallows what is behind it, per world unit. Zero is a cell with no fog.
        float mExtinction = 0.0f;

        /// One where the air is an even haze rather than banked, which is what a room holds.
        float mUniform = 0.0f;

        /// How deep the layer stands, against the bank clear weather makes in dead still air
        /// (`Shaders::VisibilityConstants::mFogLift`).
        float mLift = 1.0f;

        /// What the weather records blowing at, which carries the field downwind; the heading is
        /// the cloud deck's (`Shaders::VisibilityConstants::mFogWind`).
        float mWind = 0.0f;

        /// How far the world is built, in units, and so where the air becomes opaque. Zero is a
        /// cell with nothing cut off, which is what a room is.
        float mEdge = 0.0f;
    };

    /// What a recorded fog depth comes to as an extinction coefficient. The original engine fogs
    /// linearly from `view * (1 - depth)` to `view`, and a medium has no clear zone, so the two are
    /// matched where each is half gone: `sigma = ln(2) / (view * (1 - depth / 2))`. Clear weather's
    /// 0.69 over the game's 7168 comes to 1.476e-4. `over` is the distance the half-life is
    /// measured across — `distantLandReach` out of doors, `sInteriorFogReach` in a room — and a
    /// parameter, never a setting, because air tuned short of the world swallows the ground beyond.
    float fogExtinction(float depth, float over);

    /// What clear weather records its own land fog depth as, which every other weather's layer is
    /// a ratio of. Morrowind ships 0.69, day and night alike.
    constexpr float sClearFogDepth = 0.69f;

    /// What a wind of one adds to the layer's depth: turbulence stands a storm up out of a bank.
    /// Morrowind's recorded speeds run to 0.9, so this reaches 4.6 times the still layer; the
    /// wind cannot be the whole of the lift, because the weather named foggy has a wind of nought.
    constexpr float sFogWindLift = 4.0f;

    /// How deep a weather's layer stands, as a multiple of the one `FOG_HEIGHT` names.
    float fogLift(float depth, float wind);

    /// What the air scatters toward the eye: the sky's own light (`skyMean`, a radiance) in the
    /// weather's recorded colour (`hue`), because the record handed over as a radiance drew a foggy
    /// day the same brightness at noon and at dusk. Normalised by the brightest channel and not by
    /// luminance, because a scattering albedo cannot exceed one.
    osg::Vec3f fogColour(const osg::Vec3f& skyMean, const osg::Vec3f& hue);

    /// The distance a room's air is measured over: the view range's shipped default, stretched by
    /// the one factor here set by eye. A constant, because the original engine measures a room
    /// against `viewing distance`, so raising it thinned every cellar. A ramp is clear until
    /// `view * (1 - depth)`, further than any wall in a room, so the original draws no fog in one;
    /// unstretched, a medium lifts the room's black level by a fifth, and stretched, what is left
    /// is candlelight hanging under the chandelier.
    constexpr float sInteriorFogReach = 25.0f * 7168.0f;

    /// The open air, from the colour and the fog depth a weather is at. One place decides what
    /// `reach` (`distantLandReach`) means: the extinction is a half-life over it, the edge closes
    /// at it, and only a landscape is large enough to bank.
    Fog exteriorFog(const osg::Vec3f& colour, float depth, float wind, float reach);

    /// The same air over a cell that is built whole — Vivec's cantons, Mournhold — where the edge
    /// would close over nothing and so is not there.
    Fog quasiExteriorFog(const osg::Vec3f& colour, float depth, float wind, float reach);

    /// A room's air: measured over `sInteriorFogReach` and closing over nothing.
    Fog roomFog(const osg::Vec3f& colour, float depth);
}

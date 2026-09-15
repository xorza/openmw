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

    /// Whether some point of `cell` lies nearer than `radius` units to `eye` — the disc the rings
    /// are. Nearer and not as near, so that a cell touching the disc at its rim alone is out, which
    /// is what the bounding square `cellOf(eye ± radius)` cuts on the near side already.
    ///
    /// **A disc and not a square of cells, because the air that hides the rings' edge is a disc.**
    /// `fogEdgeAlong` closes at `distantLandReach` from the eye's own position, in every direction
    /// alike, so a cell whose nearest point is further than that is behind air that passes one
    /// part in 256 of it, and one nearer shows. A square of cells about the eye's cell loaded a
    /// corner cell that stood at 1.4 times the reach, wholly hidden, and along an axis up to a
    /// cell beyond the edge: at a reach of four that was twelve of eighty-one cells built, traced
    /// and never seen. Measured from the eye's position and not its cell, so what stands changes
    /// one cell at a time as the eye moves, which is the pace the ring adopts at anyway.
    bool withinReach(const osg::Vec2i& cell, const osg::Vec3f& eye, float radius);

    /// How far the eye stands from the nearest point of `cell`, squared: the eye itself where it
    /// stands inside the cell's square, else the eye clamped to the square's sides. What
    /// `withinReach` measures, and what orders the cells a ring asks for.
    float distanceSquaredTo(const osg::Vec2i& cell, const osg::Vec3f& eye);

    /// Calls `each(cell)` for every cell `withinReach` of `eye`, column by column and row by row:
    /// the disc's bounding square, and the disc out of it.
    template <class Each>
    void forEachCellWithin(const osg::Vec3f& eye, const float radius, Each&& each)
    {
        const osg::Vec2i low = cellOf(eye - osg::Vec3f(radius, radius, 0.0f));
        const osg::Vec2i high = cellOf(eye + osg::Vec3f(radius, radius, 0.0f));
        for (int x = low.x(); x <= high.x(); ++x)
            for (int y = low.y(); y <= high.y(); ++y)
            {
                const osg::Vec2i cell(x, y);
                if (withinReach(cell, eye, radius))
                    each(cell);
            }
    }

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

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <osg/Vec2i>
#include <osg/Vec3f>

#include <components/misc/constants.hpp>

namespace Rtx
{
    /// The fog's fractal field, drawn once into a wrapping volume with a chain of levels under it.
    /// A field a sampler reads rather than one a march computes, because value noise off a hashed
    /// lattice costs eight hashes an octave at every step of a twenty-four step march at every
    /// pixel, which is nearly the whole of a trace. The reference renderer's own trilinear value
    /// noise, one octave, wrapping on all three axes; the fractal is the shader's, which reads this
    /// at three scales that never come back into step.
    struct FogNoise
    {
        /// Every level end to end, the full one first, two channels a texel, slice by slice.
        std::vector<std::uint8_t> mBytes;

        /// Where each level begins in `mBytes`.
        std::vector<std::size_t> mOffsets;
    };

    /// Draws the tile. Every level is normalised to one mean and one spread, which is what lets the
    /// coverage band be cut against the field rather than against whichever level a step happened
    /// to reach: stretched back about the mean, what a coarse step loses is the detail and never
    /// the amount of air.
    FogNoise bakeFogNoise();

    /// One exterior cell's side, in units, as a float once for every ring that measures by it.
    inline constexpr float sCellSize = static_cast<float>(Constants::CellSizeInUnits);

    /// The cell a world position stands in, on the exterior grid.
    osg::Vec2i cellOf(const osg::Vec3f& position);

    /// Whether `cell` is within `band` cells of `eye` on both axes — the square the rings are.
    bool withinCells(const osg::Vec2i& cell, const osg::Vec2i& eye, int band);

    /// How far from the eye the world is built, in units — one number, and both the ground and the
    /// air are measured against it. Rays go everywhere, so what this path needs is how much world
    /// exists, a property of the structure and not of a camera: `viewing distance` at 7168 against
    /// a cell of 8192 barely leaves the active grid. The air follows it at both ends, because an
    /// extinction tuned to seven thousand units swallows everything past the active grid, and
    /// `Shaders::VisibilityConstants::mFogEdge` closes where `QuadTreeWorld` culls, so the ball that
    /// is built and the ball that can be seen are the same one.
    ///
    /// @param cells how many cells out to build, from `[RTX] distant land cells`. Nought hands the
    ///        decision back to the rasterizer's knob.
    /// @param viewingDistance what `[Camera] viewing distance` says, in units.
    float distantLandReach(float cells, float viewingDistance);

    /// The air in a cell, in the units the shader takes.
    struct Fog
    {
        /// What the air scatters toward the eye, linear. Out of doors this is `fogColour` of the
        /// dome's mean, which only the frame knows; a room keeps its record as it is, since there is
        /// no dome over it.
        osg::Vec3f mColour;

        /// How fast it swallows what is behind it, per world unit. Zero is a cell with no fog, and
        /// costs the shader nothing.
        float mExtinction = 0.0f;

        /// One where the air is an even haze rather than banked, which is what a room holds.
        float mUniform = 0.0f;

        /// How deep the layer stands, against the bank clear weather makes in dead still air.
        /// `Shaders::VisibilityConstants::mFogLift` says what it is for.
        float mLift = 1.0f;

        /// What the weather records blowing at, which carries the field downwind. The heading is not
        /// here: the cloud deck already carries its bearing, and `describeWorld` is where the two
        /// meet. `Shaders::VisibilityConstants::mFogWind` says the rest.
        float mWind = 0.0f;

        /// How far the world is built, in units, and so where the air becomes opaque. Zero is a
        /// cell with nothing cut off, which is what a room is.
        float mEdge = 0.0f;
    };

    /// What a recorded fog depth comes to as an extinction coefficient.
    ///
    /// The record is not a coefficient: the original engine fogs *linearly* from
    /// `view * (1 - depth)` to `view`, so a depth of 0.69 means "clear until two thousand units,
    /// gone by seven". A medium has no clear zone, so what can be matched is where each is half
    /// gone: half of the ramp is at `view * (1 - depth / 2)` and an exponential at `ln(2) / sigma`,
    /// so
    ///
    ///     sigma = ln(2) / (view * (1 - depth / 2)).
    ///
    /// Clear weather's 0.69 over the game's own 7168 comes to 1.476e-4, against the 1.5e-4 the
    /// reference renderer settled by eye. The same conversion indoors, because the original engine
    /// uses the same view range in both: a room is faint because it is small.
    ///
    /// @param over the distance the half-life is measured across — `distantLandReach` out of doors,
    ///        `sInteriorFogReach` in a room. A parameter and never a setting, because air tuned to
    ///        a shorter distance than the world is built to swallows every bit of the ground beyond.
    float fogExtinction(float depth, float over);

    /// What clear weather records its own land fog depth as: the figure every other weather is a
    /// ratio of, since `FOG_HEIGHT` is the layer clear weather makes in dead still air. Morrowind
    /// ships 0.69 for clear, day and night alike.
    constexpr float sClearFogDepth = 0.69f;

    /// What a wind of one adds to the layer's depth. Turbulence stands a storm up out of a bank;
    /// Morrowind's recorded speeds run from nought to 0.9, so this reaches 4.6 times the still
    /// layer. The wind cannot be the whole of the lift: Bethesda puts the weather named foggy at a
    /// wind of nought, so a depth driven by wind alone gave foggy the shallowest layer of the ten.
    constexpr float sFogWindLift = 4.0f;

    /// How deep a weather's layer stands, as a multiple of the one `FOG_HEIGHT` names.
    float fogLift(float depth, float wind);

    /// What the air scatters toward the eye: the sky's own light, in the weather's colour. The
    /// level belongs to the dome and only the colour to the record, because the recorded colour
    /// handed over as a radiance drew a foggy day as a flat wash the same brightness at noon and at
    /// dusk. Normalised by its brightest channel and not by its luminance, because the record is a
    /// scattering albedo and cannot exceed one — blight's (128, 19, 19) divided by its luminance
    /// came out four times the light that lit it.
    ///
    /// @param skyMean what the dome delivers on average, as a radiance — `SkyBudget::mMean`.
    /// @param hue the weather's recorded fog colour, linear.
    osg::Vec3f fogColour(const osg::Vec3f& skyMean, const osg::Vec3f& hue);

    /// The distance a room's air is measured over: the view range's shipped default, 7168, stretched
    /// by the factor here. A constant, because the original engine measures a room's ramp against
    /// `viewing distance`, so raising that setting thinned the air in every cellar in the game. The
    /// stretch is the one number here set by eye. A ramp is *clear* until `view * (1 - depth)` —
    /// 1792 units for the Seyda Neen customs office, further off than any wall in it — so the
    /// original draws that room with no fog at all, where a medium matched at its half-life puts a
    /// tenth of one between the eye and the far wall, lit by every lamp that reaches it. Unstretched,
    /// the air lifts that room's black level by a fifth; stretched, what is left is candlelight
    /// hanging under the chandelier. Nothing outdoors is stretched: aerial perspective does start at
    /// the eye.
    constexpr float sInteriorFogReach = 25.0f * 7168.0f;

    /// The open air, from the colour and the fog depth a weather is at. One place decides what the
    /// reach means: the extinction is a half-life measured over it, the edge closes at it, and only
    /// a landscape is large enough to bank.
    ///
    /// @param reach how much world is built, in units — `distantLandReach`.
    Fog exteriorFog(const osg::Vec3f& colour, float depth, float wind, float reach);

    /// The same air over a cell that is built whole: a quasi-exterior. Vivec's cantons and
    /// Mournhold are interior cells the engine runs the weather system for, so the depth and the
    /// colour are a weather's, measured over the same reach — but every wall is built, so the edge
    /// element closes over nothing and is not there.
    Fog quasiExteriorFog(const osg::Vec3f& colour, float depth, float wind, float reach);

    /// A room's air: measured over `sInteriorFogReach` and closing over nothing, because a cellar's
    /// walls are all built.
    Fog roomFog(const osg::Vec3f& colour, float depth);
}

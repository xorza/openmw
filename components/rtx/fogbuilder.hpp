#pragma once

#include <osg/Vec3f>

namespace ESM
{
    struct Cell;
}

namespace Rtx
{
    /// The air in a cell, in the units the shader takes.
    struct Fog
    {
        /// What the air scatters toward the eye, linear.
        osg::Vec3f mColour;

        /// How fast it swallows what is behind it, per world unit. Zero is a cell with no fog, and
        /// costs the shader nothing.
        float mExtinction = 0.0f;

        /// One where the air is an even haze rather than banked, which is what a room holds.
        float mUniform = 0.0f;

        /// How far the world is built, in units, and so where the air becomes opaque. Zero is a
        /// cell with nothing cut off, which is what a room is.
        /// `Shaders::VisibilityConstants::mFogEdge` says the rest.
        float mEdge = 0.0f;
    };

    /// What a recorded fog depth comes to as an extinction coefficient.
    ///
    /// **The record is not a coefficient, and this fork can read what it actually is.** The original
    /// engine fogs *linearly* between two distances taken from the view range — `FogManager` writes
    /// it out as `start = view * (1 - depth)`, `end = view` — so a depth of 0.69 means "clear until
    /// two thousand units, gone by seven". A medium has no clear zone, so the two shapes cannot be
    /// made equal; what can be matched is where each is half gone.
    ///
    /// Half of the linear ramp is at `view * (1 - depth / 2)`, and an exponential is half gone at
    /// `ln(2) / sigma`, so
    ///
    ///     sigma = ln(2) / (view * (1 - depth / 2)).
    ///
    /// Clear weather's 0.69 over the game's own 7168 comes to 1.476e-4 — against the 1.5e-4 the
    /// renderer this is ported from settled by eye, which is two routes to the same number.
    ///
    /// **And it is the same conversion indoors**, because the original engine uses the same view
    /// range in both. A room is faint because it is small, not because its dial means something
    /// different — which is the answer a separate indoor scale was standing in for.
    ///
    /// @param over the distance the half-life is measured across. **A parameter and never a
    ///        setting**, because outdoors this path builds a world to its own reach and air tuned to
    ///        a shorter one swallows every bit of it — a ring of ground four cells out then renders
    ///        identically to one none. `distantLandReach` out of doors, `sInteriorFogReach` in a
    ///        room, and neither of them moves when a player changes what they asked to see.
    float fogExtinction(float depth, float over);

    /// The distance a room's air is measured over.
    ///
    /// **A constant, because a room's mood belongs to the content and not to a graphics dial.** The
    /// original engine measures a room's ramp against `viewing distance`, so raising that setting
    /// thinned the air in every cellar in the game — which is a knob about how much world is built
    /// saying how thick the air in a windowless room is. This is that range's shipped default,
    /// 7168, stretched by the factor below, and nothing reads the live setting.
    ///
    /// **The two shapes cannot be reconciled indoors, and `fogExtinction` above says only half of
    /// why.** A ramp is *clear* until `view * (1 - depth)` — 1792 units for the Seyda Neen customs
    /// office, which is further off than any wall in it — so the original draws that room with no
    /// fog whatsoever, where a medium matched at its half-life puts a tenth of one between the eye
    /// and the far wall. A medium has no clear zone to answer that with.
    ///
    /// **And a tenth of a medium is not a tenth of a blend.** The ramp mixes a pixel toward a
    /// colour; this air is *lit*, by every lamp that reaches it — `fogLight` — so a room with two
    /// dozen candles in it scatters far more than the recorded colour ever stood for. The two are
    /// not the same quantity, which is why matching one over-delivers the other.
    ///
    /// **The stretch is the one number here set by eye**, and what it was set against is not:
    /// unstretched, the air in that customs office lifts the frame's black level to 48 of 255 and
    /// lays a grey wash over the whole room. At a stretch of ten it came to 22, which is candlelight
    /// still hanging in the air under the chandelier and nothing on the floor beneath it, and it has
    /// since been opened further — the room is what says whether it is far enough.
    ///
    /// **Nothing outdoors is stretched.** Aerial perspective does start at the eye, there is no
    /// clear zone to reproduce, and what the air scatters there is the sky — which is the colour the
    /// record already names.
    ///
    /// **One number and two hosts**: a room hazed one way in a screenshot and another in play is
    /// two renderers.
    constexpr float sInteriorFogReach = 25.0f * 7168.0f;

    /// The open air, from the colour and the fog depth a weather is at.
    ///
    /// **One place decides what the reach means.** Three of these four fields turn on how much world
    /// there is: the extinction is a half-life measured over it, the edge closes at it, and only a
    /// landscape is large enough to bank. The game and the harness reach a weather by different
    /// routes, and `FrameWorld` says what assembling a shared list on each of them costs.
    Fog exteriorFog(const osg::Vec3f& colour, float depth);

    /// A room's air, from the colour and the fog depth it is at.
    ///
    /// **Measured over a constant and closing over nothing.** A cellar's walls are all built, so
    /// there is no ring of cut ground for a second element to hide, and `sInteriorFogReach` says why
    /// the first one is not measured over the world's size either.
    Fog roomFog(const osg::Vec3f& colour, float depth);

    /// A room's air out of its `AMBI` record, for a cell the simulation is not holding open.
    Fog interiorFog(const ESM::Cell& cell);
}

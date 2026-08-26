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
    /// @param over the distance the half-life is measured across. **A parameter and not the
    ///        rasterizer's `viewing distance`**, because outdoors this path builds a world to its
    ///        own reach and air tuned to a shorter one swallows every bit of it — a ring of ground
    ///        four cells out then renders identically to one none. Indoors it stays `viewing
    ///        distance`, which is what the original engine measured a room against and what keeps a
    ///        cellar from clearing because the sky got bigger.
    float fogExtinction(float depth, float over);

    /// The distance a room's air is measured over, against the one the original engine measured it
    /// against.
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
    /// **Ten, and it is the one number here set by eye.** What it was set against is not: unstretched,
    /// the air in that customs office lifts the frame's black level to 48 of 255 and lays a grey wash
    /// over the whole room; at ten it comes to 22, which is candlelight still hanging in the air
    /// under the chandelier and nothing on the floor beneath it.
    ///
    /// **Nothing outdoors is stretched.** Aerial perspective does start at the eye, there is no
    /// clear zone to reproduce, and what the air scatters there is the sky — which is the colour the
    /// record already names.
    ///
    /// @param measured the distance the record's ramp was written against — the view range for the
    ///        harness, and for the game the midpoint of the ramp `MWRender::FogManager` has already
    ///        built. **One number and two callers**: a room hazed one way in a screenshot and
    ///        another in play is two renderers.
    float interiorFogReach(float measured);

    /// An interior's own fog, out of its `AMBI` record. Only interiors carry one; an exterior's air
    /// belongs to the weather.
    Fog interiorFog(const ESM::Cell& cell);
}

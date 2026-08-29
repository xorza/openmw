#pragma once

#include <osg/Vec3f>

namespace Rtx
{
    /// The air in a cell, in the units the shader takes.
    struct Fog
    {
        /// What the air scatters toward the eye, linear.
        ///
        /// **Out of doors this is `fogColour` of the dome's mean, and only the frame knows that
        /// mean.** A weather reader hands over the recorded colour and the frame lights it — the
        /// harness and the game both do so where their `SkyBudget` is made. A room keeps its record
        /// as it is, since there is no dome over it to light anything by.
        osg::Vec3f mColour;

        /// How fast it swallows what is behind it, per world unit. Zero is a cell with no fog, and
        /// costs the shader nothing.
        float mExtinction = 0.0f;

        /// One where the air is an even haze rather than banked, which is what a room holds.
        float mUniform = 0.0f;

        /// How deep the layer stands, against the bank clear weather makes in dead still air.
        ///
        /// `Shaders::VisibilityConstants::mFogLift` says what it is for and why the record is read
        /// twice to reach it.
        float mLift = 1.0f;

        /// What the weather records blowing at, which carries the field downwind.
        ///
        /// The heading is not here: there is one wind over a landscape and the cloud deck already
        /// carries its bearing, so `applyWorld` is where the two meet.
        /// `Shaders::VisibilityConstants::mFogWind` says the rest.
        float mWind = 0.0f;

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

    /// What clear weather records its own land fog depth as.
    ///
    /// **The figure every other weather is a ratio of.** `FOG_HEIGHT` is the layer clear weather
    /// makes in dead still air, so the depth that produced it is what turns another weather's record
    /// into a multiple of it. Morrowind ships 0.69 for clear, day and night alike.
    constexpr float sClearFogDepth = 0.69f;

    /// What a wind of one adds to the layer's depth.
    ///
    /// **Turbulence stands a storm up out of a bank**, which is the same mixing that carries the fog
    /// downwind and stirs the banks out of it. Morrowind's recorded speeds run from nought to 0.9,
    /// so this reaches 4.6 times the still layer at the top of the range.
    ///
    /// **The wind cannot be the whole of the lift, and the picture is what says so.** Bethesda puts
    /// the weather actually named foggy at a wind of nought, so a depth driven by wind alone gave
    /// foggy the shallowest layer of the ten — and a foggy morning let a player see further across
    /// Seyda Neen's bay than a clear one did. A still fog is deep and a still clear day is not, and
    /// only the recorded depth tells the two apart.
    constexpr float sFogWindLift = 4.0f;

    /// How deep a weather's layer stands, as a multiple of the one `FOG_HEIGHT` names.
    float fogLift(float depth, float wind);

    /// What the air scatters toward the eye: the sky's own light, in the weather's colour.
    ///
    /// **Fog is lit by the sky, so its level belongs to the dome and only its colour to the record.**
    /// Handing the shader the recorded colour as a radiance drew a foggy day as a flat wash the same
    /// brightness at noon and at dusk, and a night's air brighter than the night. The dome's mean is
    /// what the air is standing in, and the record says only what hue that light comes back in.
    ///
    /// **Normalised by its brightest channel and not by its luminance**, because what the record is
    /// is a scattering albedo, and an albedo cannot exceed one. Blight's `Fog Day Color` is
    /// (128, 19, 19), whose luminance is a twentieth of its red — divided by that the red came out
    /// four times the light that lit it, and the fog drowned the landscape. Against the maximum it
    /// is a deep red darker than a clear day's, which is what a blight storm looks like.
    ///
    /// @param skyMean what the dome delivers on average, as a radiance — `SkyBudget::mMean`.
    /// @param hue the weather's recorded fog colour, linear.
    osg::Vec3f fogColour(const osg::Vec3f& skyMean, const osg::Vec3f& hue);

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
    Fog exteriorFog(const osg::Vec3f& colour, float depth, float wind);

    /// A room's air, from the colour and the fog depth it is at.
    ///
    /// **Measured over a constant and closing over nothing.** A cellar's walls are all built, so
    /// there is no ring of cut ground for a second element to hide, and `sInteriorFogReach` says why
    /// the first one is not measured over the world's size either.
    Fog roomFog(const osg::Vec3f& colour, float depth);
}

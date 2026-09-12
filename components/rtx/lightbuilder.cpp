#include "lightbuilder.hpp"

#include <cmath>
#include <cstdint>

#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/vismask.hpp>

#include "decodecolour.hpp"
#include "shaders/scene.h"

namespace Rtx
{
    namespace
    {
        /// How bright a light is at half its recorded radius.
        ///
        /// There is no value in the record to be faithful to, so this is the whole of the scale and
        /// it was set by eye. Provisional in a specific way: vanilla textures have light painted
        /// into them already, so every lamp here is competing with illumination that is in the
        /// albedo, and this number only starts to mean something once that is unpicked.
        ///
        /// The pi is the Lambertian `1/pi` the shader divides by, and cancels against it exactly —
        /// so a lamp is measured on the scale below and the sun, which carries no such factor, is
        /// measured a pi apart from it. Both sides read the one constant.
        const float sIntensity = 0.25f * Shaders::PI;

        /// How much further a light reaches than its record says, and how much further again.
        ///
        /// Morrowind's radii run 64 to 256 units in an interior — a metre to three and a half at
        /// seventy units to the metre — so a lantern lights its own post and nothing else. That was
        /// a fixed falloff curve in a renderer with no bounce, where an ambient term filled the
        /// room; here the ambient is real light and the lamps have to be what lights the place.
        ///
        /// Scaling alone widens the gap it is meant to close: a candle's 64 units doubles to 128,
        /// which is still nothing, while a lantern's 256 gains a whole lantern's worth. The flat
        /// term narrows the two instead, and it is the candles that most need to leave their table.
        constexpr float sReachScale = 2.0f;
        constexpr float sReachBonus = 128.0f;

        /// How much of a lamp's recorded radius is actually alight, which is what its shadows are
        /// soft by.
        ///
        /// **The record says how far a lamp reaches and not how big it is** — but the two are
        /// already tied here, and by the line above rather than by this one. The intensity scales
        /// with the *square* of the recorded radius, which is the law for an emitter of fixed
        /// radiance whose area grows with its size; reading that same radius as a size again is
        /// that statement kept rather than a second one made, so a candle and a brazier come to
        /// differ in how soft their shadows are for the reason they already differ in how bright
        /// they are.
        ///
        /// A sixteenth across Morrowind's interior range of 64 to 256 units is a source four to
        /// sixteen units in radius — eleven to forty-six centimetres across at seventy units to the
        /// metre, which is a candle flame and a brazier's bowl. Wider and a lamp dissolves its own
        /// fixture into the shadow it casts; narrower and it is a point again.
        constexpr float sSourceFraction = 1.0f / 16.0f;

        /// How much of that same radius the fitting around the flame is assumed to fill.
        ///
        /// **A lamp is never bare.** Morrowind's lights hang inside lanterns, sit in sconces and
        /// stand in holders, and the shadow ray this buys has to leave from somewhere on the flame
        /// without ending inside the cage around it. Aimed across the flame and stopped at the flame,
        /// half the rays a wall sends end among that fitting and come back fully shadowed, which
        /// takes the whole lamp off those pixels and draws a black speckle over every lamp-lit
        /// surface in the game.
        ///
        /// Four flames of clearance is where that speckle is gone and only the ordinary grain of one
        /// sample a pixel is left, read as the share of a lit wall that is far darker than its
        /// neighbours; a lit room is brighter at every step, because what the clearance stops
        /// charging the lamp for is its own fitting.
        ///
        /// **What it costs is stated rather than hidden**: a real occluder standing closer than a
        /// quarter of a lamp's recorded radius stops casting a shadow from it. That is sixteen units
        /// for a candle and sixty-four for the largest interior lamp, and inside that distance the
        /// fitting is what an occluder nearly always is.
        constexpr float sFittingFraction = 0.25f;

        /// The top of the ladder every animation is built from, in hertz.
        ///
        /// **A flame's puffing frequency, capped by what one sample a frame can carry.** A buoyant
        /// diffusion flame sheds a vortex ring at about `1.5 / sqrt(D)` hertz, with `D` its width in
        /// metres, so a lamp flame near 28 mm across puffs at nine. Nine is also as high as this can
        /// usefully reach: the light is read once a frame, which is 6.7 samples a cycle at 60 frames a
        /// second and 3.3 at 30. Anything faster reads as noise at the first rate and aliases into a
        /// slower beat at the second.
        constexpr float sTopBand = 9.0f;

        /// One step down the ladder of bands, and the step between a fast animation and its slow twin.
        ///
        /// **The golden ratio squared, because it is irrational.** Bands at a rational ratio come back
        /// into phase and the whole flicker repeats on that period, which a viewer standing still in a
        /// lit room sees. These never do.
        constexpr float sBandRatio = 2.618034f;

        /// How many bands a flame is the sum of. Four spans a factor of eighteen in rate, which is the
        /// whole of what a flame does: the puffing at the top, and a draught wandering under it.
        constexpr int sFlameBands = 4;

        /// How far a flame swings, as a fraction of what the light radiates.
        ///
        /// This is the peak, and the bands are weighted to sum to one, so the brightness lands in
        /// `1 +- sFlameDepth` exactly. What it usually is is far smaller: with equal bands the deviation
        /// is `sFlameDepth / sqrt(2 * sFlameBands)` RMS, which is 11% of the light. A candle burning in
        /// still air varies by about a tenth of its output, and a peak three times that is the draught.
        constexpr float sFlameDepth = 0.30f;

        /// How far a pulse swings. Deeper than a flame, because a pulse is the whole of what the light
        /// does: the content gives it to lava, to glowing lichen, to Dwemer tubes and to enchanted
        /// rings, and none of those has a flame for it to be a variation of.
        constexpr float sPulseDepth = 0.35f;

        /// The slow pulse, in hertz. Three seconds a cycle reads as a swell rather than as a flicker,
        /// which is the whole difference between the two kinds.
        constexpr float sPulseBand = 1.0f / 3.0f;

        /// How far apart one light's bands are set, in turns. The golden ratio's conjugate spreads any
        /// number of them around the circle without two landing together.
        constexpr float sBandPhase = 0.618034f;

        /// Where a light stands in its animation, in turns.
        ///
        /// **Off the light's id, so that it is drawn once and never kept.** Two candles standing
        /// together must not swing as one, and a phase rolled at random would put the harness's lamp
        /// somewhere else on every run. Ids are handed out in sequence, so they are multiplied by an odd
        /// constant near the golden ratio's share of the word to scatter neighbours.
        float lightPhase(int id)
        {
            const std::uint32_t scattered = static_cast<std::uint32_t>(id) * 2654435761u;
            return static_cast<float>(scattered >> 8) * 0x1p-24f;
        }

        /// One sine of the ladder: `index` steps this light's phase along, `frequency` is in hertz.
        float band(double simulationTime, float frequency, float phase, int index)
        {
            // **Reduced to one turn in double, before it is narrowed.** A session's clock reaches tens
            // of thousands of seconds, and a float holding that many turns at nine hertz has nothing
            // left for the fraction of a turn that is the whole answer.
            const auto turns = static_cast<float>(std::fmod(static_cast<double>(frequency) * simulationTime, 1.0));

            return std::sin(2.0f * Shaders::PI * (turns + phase + static_cast<float>(index) * sBandPhase));
        }

        /// The sum of four bands of the ladder, the highest of them at `top` hertz, in `-1 .. 1`.
        float flame(double simulationTime, float top, float phase)
        {
            float sum = 0.0f;
            float frequency = top;

            for (int i = 0; i < sFlameBands; ++i)
            {
                sum += band(simulationTime, frequency, phase, i);
                frequency /= sBandRatio;
            }

            // **Equal weights, which is what makes the spectrum pink.** The bands are a geometric
            // ladder, so one weight each is one share of the power per octave — the spectrum a flame
            // has, and the reason this reads as a flame rather than as a wobble at one rate. Divided by
            // their count so that the sum cannot leave `-1 .. 1`, which is what bounds the brightness.
            return sum / static_cast<float>(sFlameBands);
        }
    }

    std::optional<Light> makeLight(const osg::Vec3f& colour, float radius, const osg::Vec3f& position)
    {
        // The radius comes off a file something else wrote, or off a graph something else built, so
        // a nonsensical one is data rather than a broken contract: a light with no size lights
        // nothing and is dropped.
        if (!(radius > 0.0f))
            return std::nullopt;

        // **A light that subtracts is not one a ray can reach.** Negative illumination is a trick
        // for a renderer accumulating into a framebuffer, and it arrives here as a colour with a
        // negative channel — which is what `SceneUtil::createLightSource` builds out of a `Negative`
        // record, and what `makeLight(const SceneUtil::LightCommon&)` builds to match. Said here, where the two
        // paths meet, so neither can come to a different answer about the same lamp.
        if (colour.x() < 0.0f || colour.y() < 0.0f || colour.z() < 0.0f)
            return std::nullopt;

        return Light{
            .mPosition = position,
            .mIntensity = colour * (radius * radius * sIntensity),
            .mReach = radius * sReachScale + sReachBonus,

            // **A sixteenth is an estimate**, and the paragraph above argues it is a good one — a
            // lamp that casts no penumbra at all is the worse answer.
            .mSourceRadius = radius * sSourceFraction,
            .mClearance = radius * sFittingFraction,
        };
    }

    float lightBrightness(SceneUtil::LightController::LightType type, int id, double simulationTime)
    {
        const float phase = lightPhase(id);

        switch (type)
        {
            case SceneUtil::LightController::LT_Normal:
                return 1.0f;
            case SceneUtil::LightController::LT_Flicker:
                // The whole flame, puffing included. The content gives this one to open fires: a
                // tiki torch, a brazier, a spark shower and a failing Dwemer tube.
                return 1.0f + sFlameDepth * flame(simulationTime, sTopBand, phase);
            case SceneUtil::LightController::LT_FlickerSlow:
                // **The same flame with its puffing damped away**, which is what a flame behind
                // lantern glass, high on a wall or across a room actually shows: the drift is left,
                // and one slower band arrives under it. The window down the ladder is the whole
                // difference between the two, and it is what makes this one cross its own mean about
                // a third as often.
                return 1.0f + sFlameDepth * flame(simulationTime, sTopBand / sBandRatio, phase);
            case SceneUtil::LightController::LT_Pulse:
                return 1.0f + sPulseDepth * band(simulationTime, sPulseBand * sBandRatio, phase, 0);
            case SceneUtil::LightController::LT_PulseSlow:
                return 1.0f + sPulseDepth * band(simulationTime, sPulseBand, phase, 0);
        }

        return 1.0f;
    }

    osg::Vec3f lightColour(const SceneUtil::LightSource& source, double simulationTime)
    {
        const SceneUtil::LightController* animation = source.getController();

        // **The controller's colours where there is one, and the light's own where there is not.**
        // Every light the content places is built by `SceneUtil::createLightSource`, which hands the
        // record's colour to a controller and lets it write the frame's colour from there; a light
        // built by hand — the glow of a Light spell — has no controller and no animation, and what
        // it was given is what it radiates.
        const SceneUtil::Light& light = *source.getLight(0);
        const osg::Vec4f diffuse = animation != nullptr ? animation->getDiffuse() : light.getDiffuse();

        const float brightness
            = animation != nullptr ? lightBrightness(animation->getType(), source.getId(), simulationTime) : 1.0f;

        // **The fade reaches the ambient and the animation does not.** The animation is a flame's,
        // and the two places the game writes an ambient both mean a light with no flame in it: a
        // Light spell's glow puts its whole output there, and a lamp carried in a pack adds a white
        // one so that it lights its bearer. The glow is why the fade has to arrive — without it a
        // Light spell burns at full strength up to the frame the actor's node mask cuts, and then
        // goes out.
        const float fade = source.getActorFade();

        return decodeColour(diffuse) * (brightness * fade) + decodeColour(light.getAmbient()) * fade;
    }

    bool castsWherePlaced(const SceneUtil::LightCommon& record)
    {
        return !record.mOffDefault;
    }

    bool standLight(osg::Group& where, const SceneUtil::LightCommon& record, bool exterior)
    {
        if (!castsWherePlaced(record))
            return false;

        // **The mirror does not filter on the mask**, so it decides nothing here. It is what the
        // game marks a light node with, so the two graphs look the same to anything that ever does.
        SceneUtil::addLight(&where, record, SceneUtil::Mask_Lighting, exterior);

        return true;
    }

    std::optional<Light> makeLight(const SceneUtil::LightCommon& record, const osg::Vec3f& position)
    {
        if (!castsWherePlaced(record))
            return std::nullopt;

        // **Described the way the graph describes it, so the test above answers for both.**
        // `SceneUtil::createLightSource` turns a `Negative` record into a light by negating its
        // diffuse, and this negates the colour it decoded. The two do that on opposite sides of the
        // sRGB conversion, so what they agree on is the *sign* and not the magnitude — which is the
        // whole of what a refusal reads, and the magnitude of a light nobody places is nothing.
        //
        // The flag is read here as what it is, a property of the record, and not as a second rule
        // about what may be placed.
        const osg::Vec3f recorded = decodeColour(record.mColor);

        return makeLight(record.mNegative ? -recorded : recorded, record.mRadius, position);
    }
}

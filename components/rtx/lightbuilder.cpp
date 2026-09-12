#include "lightbuilder.hpp"

#include <cmath>
#include <cstdint>

#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/vismask.hpp>

#include "colour.hpp"
#include "shaders/scene.h"

namespace Rtx
{
    namespace
    {
        /// How bright a light is at half its recorded radius. There is no value in the record to be
        /// faithful to, so this was set by eye, and provisionally: vanilla textures have light
        /// painted into them already. The pi is the Lambertian `1/pi` the shader divides by.
        const float sIntensity = 0.25f * Shaders::PI;

        /// How much further a light reaches than its record says, and how much further again.
        /// Morrowind's radii run 64 to 256 units in an interior, because a fixed falloff curve and
        /// an ambient term filled the room; here the lamps have to be what lights the place.
        /// Scaling alone widens the gap: a candle's 64 doubles to nothing while a lantern gains a
        /// lantern's worth, so the flat term is what lets the candles leave their table.
        constexpr float sReachScale = 2.0f;
        constexpr float sReachBonus = 128.0f;

        /// How much of a lamp's recorded radius is actually alight, which is what its shadows are
        /// soft by. The intensity already scales with the square of the recorded radius, the law
        /// for an emitter of fixed radiance whose area grows with its size, so reading that radius
        /// as a size again keeps one statement rather than making a second. A sixteenth across
        /// 64 to 256 units is four to sixteen units in radius — a candle flame and a brazier's bowl.
        constexpr float sSourceFraction = 1.0f / 16.0f;

        /// How much of that same radius the fitting around the flame is assumed to fill. A lamp is
        /// never bare, and a shadow ray stopped at the flame ends among the lantern's cage half the
        /// time, which draws a black speckle over every lamp-lit surface in the game. Four flames
        /// of clearance is where the speckle is gone. What it costs: a real occluder closer than a
        /// quarter of a lamp's recorded radius — sixteen units for a candle — stops casting a shadow
        /// from it.
        constexpr float sFittingFraction = 0.25f;

        /// The top of the ladder every animation is built from, in hertz: a buoyant diffusion flame
        /// sheds a vortex ring at about `1.5 / sqrt(D)` hertz, so a lamp flame near 28 mm across
        /// puffs at nine, which is also as high as one sample a frame can carry at 30 frames a
        /// second.
        constexpr float sTopBand = 9.0f;

        /// One step down the ladder of bands, and the step between a fast animation and its slow
        /// twin. The golden ratio squared, because bands at a rational ratio come back into phase
        /// and the whole flicker repeats.
        constexpr float sBandRatio = 2.618034f;

        /// How many bands a flame is the sum of. Four spans a factor of eighteen in rate, which is the
        /// whole of what a flame does: the puffing at the top, and a draught wandering under it.
        constexpr int sFlameBands = 4;

        /// How far a flame swings, as a fraction of what the light radiates — the peak, with the
        /// bands weighted to sum to one, so the brightness lands in `1 +- sFlameDepth` exactly. The
        /// RMS is `sFlameDepth / sqrt(2 * sFlameBands)`, 11% of the light, which is a candle in
        /// still air; the peak is the draught.
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

        /// Where a light stands in its animation, in turns, off the light's id so that it is drawn
        /// once and never kept: a phase rolled at random would put the harness's lamp somewhere
        /// else on every run. Ids are handed out in sequence, so they are scattered by an odd
        /// constant near the golden ratio's share of the word.
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

        // A light that subtracts is not one a ray can reach. It arrives here as a colour with a
        // negative channel, which is what `SceneUtil::createLightSource` builds out of a `Negative`
        // record and what the record overload builds to match.
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
                // The same flame with its puffing damped away, which is what a flame behind lantern
                // glass shows: the window down the ladder is the whole difference.
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

        // The controller's colours where there is one, and the light's own where there is not: the
        // glow of a Light spell is built by hand and has no controller.
        const SceneUtil::Light& light = *source.getLight(0);
        const osg::Vec4f diffuse = animation != nullptr ? animation->getDiffuse() : light.getDiffuse();

        const float brightness
            = animation != nullptr ? lightBrightness(animation->getType(), source.getId(), simulationTime) : 1.0f;

        // The fade reaches the ambient and the animation does not: the animation is a flame's, and
        // both places the game writes an ambient mean a light with no flame in it. Without the fade
        // a Light spell burns at full strength up to the frame the actor's node mask cuts.
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

        // Described the way the graph describes it, so the test above answers for both: the two
        // negate on opposite sides of the sRGB conversion, so what they agree on is the sign, which
        // is the whole of what a refusal reads.
        const osg::Vec3f recorded = decodeColour(record.mColor);

        return makeLight(record.mNegative ? -recorded : recorded, record.mRadius, position);
    }
}

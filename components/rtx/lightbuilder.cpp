#include "lightbuilder.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <span>

#include <osg/BoundingBox>
#include <osg/BoundingSphere>
#include <osg/Matrixf>
#include <osg/Vec4f>

#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightmanager.hpp>

#include "colour.hpp"
#include "finite.hpp"
#include "material.hpp"
#include "mesh.hpp"
#include "shaders/look.h"
#include "shaders/scene.h"
#include "sprite.hpp"

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

        /// How high over the actor's feet, where the game hangs a Light spell's glow, its lamp
        /// stands: half of the 128 units Morrowind's people stand, so the lamp is inside the body
        /// and lights everything round it but the body itself, which faces away from it.
        constexpr float sSpellLightLift = 64.0f;

        /// How much brighter a burst's lamp is than the shell and the discs it is derived from,
        /// and how far it reaches, in radii of its ball.
        ///
        /// **Derived is the shape and by eye is the level**, like `sIntensity`: the derivation
        /// says how one burst stands to another and to its own size, and nothing in it says how
        /// a burst stands to the lamps of a room, because the room's lamps were set by eye too. At
        /// one and four radii a burst was judged too dim and too short, and at four and eight
        /// still too short: `falloff` windows a lamp to nought at its reach, so a reach is where
        /// the pool is cut and not where it fades, and sixteen radii puts the cut where an
        /// inverse square at four times the intensity has fallen to a quarter of the level the
        /// four-radius pool was cut at: `4 / 16^2` against `1 / 4^2`.
        constexpr float sGlowGain = 4.0f;
        constexpr float sGlowReachScale = 16.0f;

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
            // Reduced to one turn in double, before it is narrowed. A session's clock reaches tens
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

            // Equal weights, which is what makes the spectrum pink. The bands are a geometric
            // ladder, so one weight each is one share of the power per octave — the spectrum a flame
            // has, and the reason this reads as a flame rather than as a wobble at one rate. Divided by
            // their count so that the sum cannot leave `-1 .. 1`, which is what bounds the brightness.
            return sum / static_cast<float>(sFlameBands);
        }

        /// The lamp, where every number in it is finite. What it was built from came off a file or
        /// off a graph something else built, so a number that is not finite is data and the lamp is
        /// refused: the grid sized around it would double its cell for ever, and the light would
        /// shade every surface it reached to NaN.
        Result<std::optional<Light>, std::string_view> finiteOnly(const Light& lamp)
        {
            if (!isFinite(lamp.mPosition) || !isFinite(lamp.mIntensity) || !std::isfinite(lamp.mReach)
                || !std::isfinite(lamp.mSourceRadius) || !std::isfinite(lamp.mClearance))
                return Err{ "a number it is made of is not finite" };

            return lamp;
        }
    }

    Result<std::optional<Light>, std::string_view> makeLight(
        const osg::Vec3f& colour, float radius, const osg::Vec3f& position)
    {
        // A light of no size lights nothing in the game either. One that is not a number at all
        // goes on to be refused with the rest of what is not finite.
        if (std::isfinite(radius) && radius <= 0.0f)
            return std::nullopt;

        // A light that subtracts is not one a ray can reach. It arrives here as a colour with a
        // negative channel, which is what `SceneUtil::createLightSource` builds out of a `Negative`
        // record and what the record overload builds to match.
        if (colour.x() < 0.0f || colour.y() < 0.0f || colour.z() < 0.0f)
            return Err{ "it takes light away, which a ray cannot" };

        return finiteOnly(Light{
            .mPosition = position,
            .mIntensity = colour * (radius * radius * sIntensity),
            .mReach = radius * sReachScale + sReachBonus,

            // A sixteenth is an estimate, and the paragraph above argues it is a good one — a
            // lamp that casts no penumbra at all is the worse answer.
            .mSourceRadius = radius * sSourceFraction,
            .mClearance = radius * sFittingFraction,
        });
    }

    Result<std::optional<Light>, std::string_view> makeSpellLight(
        const osg::Vec3f& colour, const float radius, const osg::Vec3f& position)
    {
        const osg::Vec3f clamped(std::min(colour.x(), 1.0f), std::min(colour.y(), 1.0f), std::min(colour.z(), 1.0f));
        return makeLight(clamped, radius, position + osg::Vec3f(0.0f, 0.0f, sSpellLightLift));
    }

    bool isSpellLight(const SceneUtil::LightSource& source)
    {
        const SceneUtil::Light& light = *source.getLight(0);
        const osg::Vec4f diffuse = light.getDiffuse();
        const osg::Vec4f ambient = light.getAmbient();

        return diffuse.x() == 0.0f && diffuse.y() == 0.0f && diffuse.z() == 0.0f
            && (ambient.x() > 0.0f || ambient.y() > 0.0f || ambient.z() > 0.0f);
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

    void Glow::addSheet(const Material& worn, const osg::BoundingBoxf& box, const osg::Matrixf& place, const float fade)
    {
        if (!worn.isAdditive() || !box.valid())
            return;

        // What the crossing's alpha weighs, as `additiveAlong` weighs it: nothing under a blend
        // that adds whole, and the material's opacity under the instance's fade otherwise. The
        // texel's own alpha is already in the mean.
        const float opacity = worn.mBlend == BlendKind::AddWhole ? 1.0f : worn.mOpacity * fade;

        const osg::Vec3f tinted = osg::componentMultiply(worn.mDiffuseMean, worn.mDiffuseColour) * opacity;
        mRadiance += osg::componentMultiply(tinted, worn.mEmissiveColour) * Shaders::EMISSIVE_INTENSITY;

        const osg::Vec3f half = (box._max - box._min) * 0.5f;
        const float radius = std::max({ half.x(), half.y(), half.z() }) * placedScale(place);
        const osg::BoundingSpheref stood(box.center() * place, radius);
        mSheets.expandBy(stood);
        mBall.expandBy(stood);
    }

    void Glow::addSprites(const SpriteEmitter& emitter, const std::span<const Sprite> sprites, const osg::Vec3f& mean)
    {
        assert(sprites.size() == emitter.mCount && "an emitter handed sprites that are not its own");

        if (!emitter.isAdditive() || sprites.empty())
            return;

        // `r^2 * L` a sprite, with the pi of the disc's area put on once by `makeLight`.
        osg::Vec3f discs;
        for (const Sprite& sprite : sprites)
            discs += osg::componentMultiply(mean, sprite.mColour) * (sprite.mAlpha * sprite.mRadius * sprite.mRadius);

        mDiscs += discs * Shaders::SUNLIT_WHITE;
        mBall.expandBy(osg::BoundingSpheref(emitter.mCentre, emitter.mReach));
    }

    Result<std::optional<Light>, std::string_view> Glow::makeLight() const
    {
        // An effect of nothing, or of a point, glows nothing; a ball that is not a number is not
        // `valid` either, and goes on to be refused with the rest of what is not finite.
        if (mLit || mBall.radius() <= 0.0f)
            return std::nullopt;

        osg::Vec3f intensity = mDiscs * Shaders::PI;
        if (mSheets.valid())
            intensity += mRadiance * (2.0f * Shaders::PI * mSheets.radius() * mSheets.radius());

        if (intensity == osg::Vec3f())
            return std::nullopt;

        // **A fill.** The ball is the source, so the shadow ray opens to the whole of it, and the
        // clearance too, so the ray stops at the ball and nothing inside it casts a shadow;
        // `weighLamps` reads `mFill` to light what is inside from every side.
        const float radius = mBall.radius();
        return finiteOnly(Light{
            .mPosition = mBall.center(),
            .mIntensity = intensity * sGlowGain,
            .mReach = radius * sGlowReachScale,
            .mSourceRadius = radius,
            .mClearance = radius,
            .mFill = 1,
        });
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

    SceneUtil::LightController::LightType animationOf(const SceneUtil::LightCommon& record)
    {
        SceneUtil::LightController::LightType type = SceneUtil::LightController::LT_Normal;
        if (record.mFlicker)
            type = SceneUtil::LightController::LT_Flicker;
        if (record.mFlickerSlow)
            type = SceneUtil::LightController::LT_FlickerSlow;
        if (record.mPulse)
            type = SceneUtil::LightController::LT_Pulse;
        if (record.mPulseSlow)
            type = SceneUtil::LightController::LT_PulseSlow;

        return type;
    }

    Result<std::optional<Light>, std::string_view> makeLight(
        const SceneUtil::LightCommon& record, const osg::Vec3f& position, const double simulationTime, const int id)
    {
        if (!castsWherePlaced(record))
            return std::nullopt;

        // Described the way the graph describes it, so the test above answers for both: the two
        // negate on opposite sides of the sRGB conversion, so what they agree on is the sign, which
        // is the whole of what a refusal reads.
        const osg::Vec3f recorded = decodeColour(record.mColor);
        const float brightness = lightBrightness(animationOf(record), id, simulationTime);

        // The minimum scene light radius is 16 in Morrowind, which `createLightSource` applies
        // before the walk ever reads the source's radius back.
        const float radius = std::max(record.mRadius, 16.0f);

        return makeLight((record.mNegative ? -recorded : recorded) * brightness, radius, position);
    }
}

#include "moonbuilder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include <osg/Quat>

#include <components/fallback/fallback.hpp>
#include <components/sky/moonmodel.hpp>
#include <components/vfs/pathutil.hpp>

#include "error.hpp"
#include "lightbuilder.hpp"
#include "shaders/colour.h"
#include "shaders/look.h"
#include "shaders/scene.h"

namespace Rtx
{
    namespace
    {
        std::string_view nameOf(Moon moon)
        {
            return moon == Moon::Masser ? "Masser" : "Secunda";
        }

        float setting(Moon moon, std::string_view field)
        {
            return Fallback::Map::getFloat("Moons_" + std::string(nameOf(moon)) + "_" + std::string(field));
        }

        /// The same, refusing a reading of nought.
        ///
        /// **`Fallback::Map` answers an allowed key nobody planted with a silent zero**, and both
        /// readings below are held for the life of the process — so a moon asked for before the
        /// settings were read would leave every sky after it wrong, with nothing to say why. None of
        /// the quantities this guards is one a moon can have at nought.
        float requireSetting(Moon moon, std::string_view field)
        {
            const float value = setting(moon, field);
            if (!(value > 0.0f))
                throw Error("Moons_" + std::string(nameOf(moon)) + "_" + std::string(field)
                    + " is nought: the moons were asked for before the settings that describe them were read");

            return value;
        }

        /// The mean opaque texel of `tx_masser_full.dds` and `tx_secunda_full.dds`, linear.
        ///
        /// Measured off the shipped portraits rather than chosen: one red, one grey, and the ratio
        /// between them is what tells the two moons apart at a glance.
        const osg::Vec3f sMasserFace(0.0332f, 0.0099f, 0.0123f);
        const osg::Vec3f sSecundaFace(0.0440f, 0.0373f, 0.0295f);

        /// Masser's own luminance. The two tints are normalised on it, so that `Shaders::MOON_ALBEDO`
        /// is the albedo of exactly one moon rather than of an average of two.
        const float sMasserLuma = sMasserFace * Shaders::LUMINANCE_WEIGHTS;

        /// Half the angle a moon of this size subtends, out of the geometry the game's own renderer
        /// builds: `Moons_<name>_Size / 125 * 450` scales a quad of half-extent 0.5, a thousand
        /// units away.
        float subtendedBy(Moon moon)
        {
            const float halfWidth = 0.5f * 450.0f * requireSetting(moon, "Size") / 125.0f;
            return std::atan(halfWidth / 1000.0f);
        }

        /// The same, worked out once.
        ///
        /// **Both moons are placed every frame**, and `setting` builds two strings to look a key up
        /// with — an allocation on the frame path, for a pair of numbers that are fixed for the run.
        float angularRadiusOf(Moon moon)
        {
            static const float sMasser = subtendedBy(Moon::Masser);
            static const float sSecunda = subtendedBy(Moon::Secunda);

            return moon == Moon::Masser ? sMasser : sSecunda;
        }

        /// This moon's clock, read out of its ten `Moons_*` settings.
        ///
        /// **The speed is asked for first, and `requireSetting` says why.** A `MoonModel` reads all
        /// ten without judging any of them, so nothing else here would notice a clock built out of
        /// nothing — and `clockOf` holds it for the run.
        Sky::MoonModel readClock(Moon moon)
        {
            requireSetting(moon, "Speed");

            return Sky::MoonModel(nameOf(moon));
        }

        /// This moon's clock, built once.
        ///
        /// **`MoonModel`'s constructor reads ten `Moons_*` settings by name**, each of them a key
        /// built on the spot, and both moons are placed on every frame. What it holds is fixed for
        /// the run: `at` is given the day and the hour and reads nothing else.
        const Sky::MoonModel& clockOf(Moon moon)
        {
            static const Sky::MoonModel sMasser = readClock(Moon::Masser);
            static const Sky::MoonModel sSecunda = readClock(Moon::Secunda);

            return moon == Moon::Masser ? sMasser : sSecunda;
        }

        /// This moon's colour, on a scale where Masser's luminance is one.
        ///
        /// **The difference in brightness is kept and only the level is taken out.** Secunda's
        /// portrait averages two and a half times Masser's, and that is a fact about the two bodies
        /// rather than an accident of the art — a pale moon reflects more of the same sunlight than
        /// a dark red one.
        osg::Vec3f tintOf(Moon moon)
        {
            return (moon == Moon::Masser ? sMasserFace : sSecundaFace) / sMasserLuma;
        }

        /// What a full moon of this size delivers to a surface facing it, before its own tint.
        ///
        /// **A disc of geometric albedo `p` and half-angle `t` under irradiance `E` delivers
        /// `E * p * sin(t)^2`.** Which is `L * pi * sin(t)^2` for a disc of radiance `L`, with
        /// `L = E * p / pi` for the Lambertian body a full moon is — so the light is the sun, the
        /// albedo and the sky the moon covers, and nothing else. `Shaders::MOON_ALBEDO` carries why
        /// none of the three is a number this renderer chose.
        ///
        /// Taken from the angle rather than from the `Size` setting behind it, so that a moon hung
        /// at a distance of its own would still come out right.
        float deliveredBy(Moon moon)
        {
            const float sine = std::sin(angularRadiusOf(moon));

            return Shaders::DAYLIGHT * Shaders::MOON_ALBEDO * sine * sine;
        }

        /// How much light a moon at `phaseAngle` sends, against a full one.
        ///
        /// **The measured law rather than the geometry, and the two differ by a factor of five.**
        /// The lit share of a disc is `(1 + cos a) / 2`, which makes a half moon half of a full one.
        /// The moon is not: its surface is rough enough to shadow itself everywhere but at
        /// opposition. Allen's fit to the observations — `dm = 0.026|a| + 4e-9 a^4`, with `a` in
        /// degrees — puts a half moon at 0.09 of full, which is what photometry finds, and its
        /// quartic term is the opposition surge that makes the last nights before full so much
        /// brighter than the rest.
        ///
        /// Folded through the cosine, because a moon three quarters round its cycle is as lit as one
        /// a quarter round it. Which limb keeps the light is the disc's business and not the light's.
        float phaseLaw(float phaseAngle)
        {
            const float from = std::acos(std::clamp(std::cos(phaseAngle), -1.0f, 1.0f));
            const float degrees = osg::RadiansToDegrees(from);
            const float dim = 0.026f * degrees + 4.0e-9f * degrees * degrees * degrees * degrees;

            return std::pow(10.0f, -0.4f * dim);
        }
    }

    MoonFaces addMoonFaces(SceneDesc& scene)
    {
        constexpr VFS::Path::NormalizedView masser("textures/tx_masser_full.dds");
        constexpr VFS::Path::NormalizedView secunda("textures/tx_secunda_full.dds");

        const MoonFaces faces{ .mMasser = scene.addTexture(masser), .mSecunda = scene.addTexture(secunda) };
        scene.holdTexture(faces.mMasser);
        scene.holdTexture(faces.mSecunda);
        return faces;
    }

    Shaders::MoonDisc describeMoon(const MoonPlacement& placement)
    {
        return Shaders::MoonDisc{
            .mDirection = placement.mDirection,
            .mRight = placement.mRight,
            .mUp = placement.mUp,
            .mColour = placement.mColour,
            .mIrradiance = placement.mIrradiance,
            .mLimb = std::sin(placement.mAngularRadius),
            .mPhaseAngle = placement.mPhaseAngle,
            .mAlpha = placement.mAlpha,
            .mThroughAir = placement.mThroughAir,
            .mFace = placement.mFace == sNoIndex ? Shaders::NO_TEXTURE : static_cast<std::uint32_t>(placement.mFace),
        };
    }

    float moonAngularRadius(Moon moon)
    {
        return angularRadiusOf(moon);
    }

    MoonPlacement makeMoon(Moon moon, int day, float hour, float glare)
    {
        const Sky::MoonMoment moment = clockOf(moon).at(day, hour);

        return placeMoon(
            moon, moment.mAlongArc, moment.mAxisOffset, static_cast<int>(moment.mPhase), moment.mDaylightFade * glare);
    }

    MoonPlacement placeMoon(Moon moon, float alongArcDegrees, float axisOffsetDegrees, int phase, float alpha)
    {
        // `Moon::setState`'s own two rotations (`apps/openmw/mwrender/gl/skyutil.cpp:900`): the arc
        // tips the moon up from the horizon about +X, and the axis offset swings that whole arc
        // about the zenith so the two moons rise in different places and their paths cross.
        const float alongArc = osg::DegreesToRadians(alongArcDegrees);
        const float aboutZenith = osg::DegreesToRadians(axisOffsetDegrees);

        const osg::Quat arc(alongArc, osg::Vec3f(1.0f, 0.0f, 0.0f));
        const osg::Quat swing(aboutZenith, osg::Vec3f(0.0f, 0.0f, 1.0f));

        // **The face's own attitude, and not a billboard's.** The quad the game draws starts facing
        // down, so its rotation carries the same quarter turn — which is what leaves the portrait
        // upright against the moon's arc rather than against the horizon.
        const osg::Quat attitude = osg::Quat(alongArc - 0.5f * osg::PIf, osg::Vec3f(1.0f, 0.0f, 0.0f)) * swing;

        MoonPlacement placement{
            .mDirection = arc * swing * osg::Vec3f(0.0f, 1.0f, 0.0f),
            .mRight = attitude * osg::Vec3f(1.0f, 0.0f, 0.0f),
            .mUp = attitude * osg::Vec3f(0.0f, 1.0f, 0.0f),
            .mAngularRadius = angularRadiusOf(moon),

            // Eight painted phases are eight steps of a half turn each way, counted from full — so
            // the index is the angle, and the sign of its sine is the limb the light is on.
            .mPhaseAngle = static_cast<float>(phase) * 0.25f * osg::PIf,

            // **Nought until it is on its arc**, which the engine states by leaving the angle there
            // until a moon rises and returning it there once it sets. Without this a moon that is
            // down sits on the horizon all night, because nothing else in the placement says so.
            .mAlpha = alongArcDegrees > 0.0f ? alpha : 0.0f,

            // **The file's own mean, unscaled.** `Shaders::MOON_RADIANCE` is what takes a
            // moon's texels to radiance, and it multiplies this where no portrait is loaded and the
            // portrait itself where one is — so the level lives in one place either way.
            .mColour = moon == Moon::Masser ? sMasserFace : sSecundaFace,
        };

        placement.mDirection.normalize();
        placement.mRight.normalize();
        placement.mUp.normalize();

        // **The air, over what it shows and what it sends alike**, and read off the direction rather
        // than off the arc: the slant path is measured on an elevation, and the two agree only
        // because this arc runs through the zenith.
        placement.mThroughAir = airTransmittance(placement.mDirection.z());

        const osg::Vec3f lit = tintOf(moon) * (deliveredBy(moon) * phaseLaw(placement.mPhaseAngle) * placement.mAlpha);
        placement.mIrradiance = osg::componentMultiply(lit, placement.mThroughAir);

        return placement;
    }
}

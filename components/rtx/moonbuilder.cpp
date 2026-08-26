#include "moonbuilder.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <osg/Quat>

#include <components/fallback/fallback.hpp>
#include <components/sky/moonmodel.hpp>
#include <components/vfs/pathutil.hpp>

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

        /// The mean opaque texel of `tx_masser_full.dds` and `tx_secunda_full.dds`, linear.
        ///
        /// Measured off the shipped portraits rather than chosen: one red, one grey, and the ratio
        /// between them is what tells the two moons apart at a glance.
        const osg::Vec3f sMasserFace(0.0332f, 0.0099f, 0.0123f);
        const osg::Vec3f sSecundaFace(0.0440f, 0.0373f, 0.0295f);

        /// Half the angle a moon of this size subtends, out of the geometry the game's own renderer
        /// builds: `Moons_<name>_Size / 125 * 450` scales a quad of half-extent 0.5, a thousand
        /// units away.
        float angularRadiusOf(Moon moon)
        {
            const float halfWidth = 0.5f * 450.0f * setting(moon, "Size") / 125.0f;
            return std::atan(halfWidth / 1000.0f);
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

    void dropMoonFaces(SceneDesc& scene, const MoonFaces& faces)
    {
        scene.dropTexture(faces.mMasser);
        scene.dropTexture(faces.mSecunda);
    }

    Shaders::MoonDisc describeMoon(const MoonPlacement& placement)
    {
        return Shaders::MoonDisc{
            .mDirection = placement.mDirection,
            .mRight = placement.mRight,
            .mUp = placement.mUp,
            .mColour = placement.mColour,
            .mAngularRadius = placement.mAngularRadius,
            .mPhaseAngle = placement.mPhaseAngle,
            .mAlpha = placement.mAlpha,
            .mFace = placement.mFace == sNoIndex ? Shaders::NO_TEXTURE : static_cast<std::uint32_t>(placement.mFace),
        };
    }

    float moonAngularRadius(Moon moon)
    {
        return angularRadiusOf(moon);
    }

    MoonPlacement makeMoon(Moon moon, int day, float hour)
    {
        const Sky::MoonMoment moment = Sky::MoonModel(nameOf(moon)).at(day, hour);

        return placeMoon(moon, moment.mAlongArc, moment.mAxisOffset, static_cast<int>(moment.mPhase), moment.mAlpha);
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

            .mAlpha = alpha,

            // **The file's own mean, unscaled.** `Shaders::MOON_RADIANCE` is what takes a
            // moon's texels to radiance, and it multiplies this where no portrait is loaded and the
            // portrait itself where one is — so the level lives in one place either way.
            .mColour = moon == Moon::Masser ? sMasserFace : sSecundaFace,
        };

        placement.mDirection.normalize();
        placement.mRight.normalize();
        placement.mUp.normalize();
        return placement;
    }
}

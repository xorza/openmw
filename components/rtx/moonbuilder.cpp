#include "moonbuilder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include <osg/Image>
#include <osg/Math>
#include <osg/Quat>
#include <osg/ref_ptr>

#include "refusals.hpp"
#include "result.hpp"
#include "scenedesc.hpp"
#include "shaders/colour.h"
#include "shaders/look.h"
#include "shaders/scene.h"
#include "shaders/sky.h"
#include "skylight.hpp"
#include "texturebuilder.hpp"
#include "texturewrap.hpp"

namespace Rtx
{
    namespace
    {
        std::string_view nameOf(Moon moon)
        {
            return moon == Moon::Masser ? "Masser" : "Secunda";
        }

        /// The mean opaque texel of `tx_masser_full.dds` and `tx_secunda_full.dds`, linear —
        /// measured off the shipped portraits rather than chosen: one red, one grey, and the ratio
        /// between them is what tells the two moons apart at a glance.
        const osg::Vec3f sMasserFace(0.0332f, 0.0099f, 0.0123f);
        const osg::Vec3f sSecundaFace(0.0440f, 0.0373f, 0.0295f);

        /// Masser's own luminance. The two tints are normalised on it, so that `Shaders::MOON_ALBEDO`
        /// is the albedo of exactly one moon rather than of an average of two.
        const float sMasserLuma = sMasserFace * Shaders::LUMINANCE_WEIGHTS;

        /// How wide `size` draws `moon`. `Fallback::Map` answers a key the configuration leaves out,
        /// and one that does not parse, with nought, and the game draws a quad of no extent for
        /// that; an error for a size below nought, or one that is not finite.
        Result<float, std::string> radiusOf(Moon moon, float size)
        {
            if (!(size >= 0.0f) || !std::isfinite(size))
                return Err{ "Moons_" + std::string(nameOf(moon)) + "_Size is " + std::to_string(size)
                    + ", which is no size" };

            return moonAngularRadius(size);
        }

        /// This moon's colour, on a scale where Masser's luminance is one: Secunda's portrait
        /// averages two and a half times Masser's, and a pale moon reflects more of the same
        /// sunlight than a dark red one.
        osg::Vec3f tintOf(Moon moon)
        {
            return (moon == Moon::Masser ? sMasserFace : sSecundaFace) / sMasserLuma;
        }

        /// What a full moon of `angularRadius` delivers to a surface facing it, before its own tint:
        /// a disc of geometric albedo `p` and half-angle `t` under irradiance `E` delivers
        /// `E * p * sin(t)^2`. Taken from the angle rather than from the `Size` setting behind it.
        float deliveredBy(float angularRadius)
        {
            const float sine = std::sin(angularRadius);

            return Shaders::DAYLIGHT * Shaders::MOON_ALBEDO * sine * sine;
        }

        /// How much light a moon at `phaseAngle` sends, against a full one — the measured law and
        /// not the geometry, which differ by a factor of five because the surface shadows itself
        /// everywhere but at opposition. Allen's fit, `dm = 0.026|a| + 4e-9 a^4` in degrees, puts a
        /// half moon at 0.09 of full. Folded through the cosine, because which limb keeps the light
        /// is the disc's business.
        float phaseLaw(float phaseAngle)
        {
            const float from = std::acos(std::clamp(std::cos(phaseAngle), -1.0f, 1.0f));
            const float degrees = osg::RadiansToDegrees(from);
            const float dim = 0.026f * degrees + 4.0e-9f * degrees * degrees * degrees * degrees;

            return std::pow(10.0f, -0.4f * dim);
        }
    }

    MoonFaces addMoonFaces(SceneDesc& scene, Resource::ImageManager& images, const MoonSizes& sizes)
    {
        // A moon of a size that is no size is refused and not drawn.
        const auto drawnWidth = [&](Moon moon, float size) {
            const Result<float, std::string> radius = radiusOf(moon, size);
            if (radius.isOk())
                return radius.value();

            scene.refusals().refuse(Refused::Moon, nameOf(moon), radius.error());
            return 0.0f;
        };

        // Clamped: a portrait is one image edge to edge, and a repeating tap at its limb would
        // blend the far edge's paint into the disc's antialiasing. A face that does not open takes
        // its slot with no image, which the upload stands in for and refuses; the image manager
        // logged why.
        const auto face = [&](const Moon moon) {
            const VFS::Path::NormalizedView path = moonFaceOf(moon);
            const Result<osg::ref_ptr<const osg::Image>, std::string> image = openImage(images, path);
            const Index slot
                = scene.textures().add(path, image.isOk() ? image.value().get() : nullptr, TextureWrap::Clamp);
            scene.textures().hold(slot);
            return slot;
        };

        return MoonFaces{ .mMasser = face(Moon::Masser),
            .mSecunda = face(Moon::Secunda),
            .mMasserRadius = drawnWidth(Moon::Masser, sizes.mMasser),
            .mSecundaRadius = drawnWidth(Moon::Secunda, sizes.mSecunda) };
    }

    void dropMoonFaces(SceneDesc& scene, const MoonFaces& faces)
    {
        scene.textures().drop(faces.mMasser);
        scene.textures().drop(faces.mSecunda);
    }

    Shaders::MoonDisc describeMoon(const MoonPlacement& placement)
    {
        return Shaders::MoonDisc{
            .mSource
            = Shaders::moonSource(placement.mDirection, placement.getPaintedIrradiance(), placement.mAngularRadius),
            .mRight = placement.mRight,
            .mUp = placement.mUp,
            .mColour = placement.mColour,
            .mPhaseAngle = placement.mPhaseAngle,
            .mAlpha = placement.mAlpha,
            .mThroughAir = placement.mThroughAir,
            .mPaint = placement.mPaint,
            .mFace = placement.mFace == sNoIndex ? Shaders::NO_TEXTURE : static_cast<std::uint32_t>(placement.mFace),
        };
    }

    float moonAngularRadius(float size)
    {
        if (!(size > 0.0f) || !std::isfinite(size))
            return 0.0f;

        const float halfWidth = 0.5f * 450.0f * size / 125.0f;
        return std::atan(halfWidth / 1000.0f);
    }

    MoonPlacement placeMoon(const MoonFaces& faces, Moon moon, float alongArcDegrees, float axisOffsetDegrees,
        Sky::MoonPhase phase, float alpha)
    {
        const float angularRadius = faces.radiusOf(moon);

        // `Moon::setState`'s own two rotations (`apps/openmw/mwrender/skyutil.cpp:900`): the arc
        // tips the moon up from the horizon about +X, and the axis offset swings that whole arc
        // about the zenith so the two moons rise in different places and their paths cross.
        const float alongArc = osg::DegreesToRadians(alongArcDegrees);
        const float aboutZenith = osg::DegreesToRadians(axisOffsetDegrees);

        const osg::Quat arc(alongArc, osg::Vec3f(1.0f, 0.0f, 0.0f));
        const osg::Quat swing(aboutZenith, osg::Vec3f(0.0f, 0.0f, 1.0f));

        // The face's own attitude, and not a billboard's. The quad the game draws starts facing
        // down, so its rotation carries the same quarter turn — which is what leaves the portrait
        // upright against the moon's arc rather than against the horizon.
        const osg::Quat attitude = osg::Quat(alongArc - 0.5f * osg::PIf, osg::Vec3f(1.0f, 0.0f, 0.0f)) * swing;

        MoonPlacement placement{
            .mDirection = arc * swing * osg::Vec3f(0.0f, 1.0f, 0.0f),
            .mRight = attitude * osg::Vec3f(1.0f, 0.0f, 0.0f),
            .mUp = attitude * osg::Vec3f(0.0f, 1.0f, 0.0f),
            .mAngularRadius = angularRadius,

            // Eight painted phases are eight steps of a half turn each way, counted from full — so
            // the index is the angle, and the sign of its sine is the limb the light is on.
            .mPhaseAngle = static_cast<float>(phase) * 0.25f * osg::PIf,

            // Nought until it is on its arc, which the engine states by leaving the angle there
            // until a moon rises and returning it there once it sets. Without this a moon that is
            // down sits on the horizon all night, because nothing else in the placement says so.
            // Nought too for a moon of no size, whose disc the sky would measure by a limb of zero.
            .mAlpha = alongArcDegrees > 0.0f && angularRadius > 0.0f ? alpha : 0.0f,
            .mFace = faces.of(moon),

            // The file's own mean, unscaled. `Shaders::MOON_RADIANCE` is what takes a
            // moon's texels to radiance, and it multiplies this where no portrait is loaded and the
            // portrait itself where one is — so the level lives in one place either way.
            .mColour = moon == Moon::Masser ? sMasserFace : sSecundaFace,
        };

        placement.mDirection.normalize();
        placement.mRight.normalize();
        placement.mUp.normalize();

        // The air, over what it shows and what it sends alike, and read off the direction rather
        // than off the arc: the slant path is measured on an elevation, and the two agree only
        // because this arc runs through the zenith.
        placement.mThroughAir = airTransmittance(placement.mDirection.z());

        const osg::Vec3f lit
            = tintOf(moon) * (deliveredBy(angularRadius) * phaseLaw(placement.mPhaseAngle) * placement.mAlpha);
        placement.mIrradiance = osg::componentMultiply(lit, placement.mThroughAir);

        return placement;
    }
}

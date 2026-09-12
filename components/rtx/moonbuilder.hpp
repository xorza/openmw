#pragma once

#include <osg/Vec3f>

#include <components/sky/moonstate.hpp>

#include "runs.hpp"
#include "shaders/look.h"
#include "shaders/visibility.h"

namespace Rtx
{
    class SceneDesc;

    /// Which of the two moons over Vvardenfell.
    enum class Moon
    {
        Masser,
        Secunda,
    };

    /// Where a moon stands, how big it is, and how much of it the sun has — a disc rather than a
    /// body, found by a ray that reaches nothing: a direction, an angular size, and the two axes
    /// its face is painted along.
    struct MoonPlacement
    {
        /// Unit vector toward the moon.
        osg::Vec3f mDirection;

        /// The face's own axes, unit and perpendicular to `mDirection` and to each other: `mRight`
        /// runs along the portrait's `u` and `mUp` against its `v`. A moon is not a billboard: the
        /// portrait turns against the horizon as the moon crosses, as `Moon::setState` builds it.
        osg::Vec3f mRight;
        osg::Vec3f mUp;

        /// Half the angle the disc subtends, in radians.
        float mAngularRadius = 0.0f;

        /// How far round its cycle the moon is, in radians: zero is full and pi is new. The lit
        /// share of the face is `(1 + cos) / 2`, and the sign of the sine says which limb keeps it.
        float mPhaseAngle = 0.0f;

        /// What the game fades the moon out by near the horizon and around the ends of its arc, from
        /// zero to one. Zero is a moon that is not there to be drawn.
        float mAlpha = 0.0f;

        /// The painted face in the scene's texture table, or `sNoIndex` for none.
        Index mFace = sNoIndex;

        /// What this moon delivers to a surface facing it, linear. Zero exactly where the fade is,
        /// which is the one test a shader makes before it spends a shadow ray; a new moon is three
        /// parts in ten thousand, which is what the photometry says.
        osg::Vec3f mIrradiance;

        /// What the air leaves of it, per channel — `Rtx::airTransmittance` at its own elevation,
        /// which is what lets a moon rise where the engine draws none under
        /// `Moons_<name>_Fade_End_Angle`. The light is dimmed by it too, on the host.
        osg::Vec3f mThroughAir{ 1.0f, 1.0f, 1.0f };

        /// The mean opaque texel of this moon's portrait, linear and unscaled — what the disc falls
        /// back to where no portrait is loaded. `Shaders::MOON_RADIANCE` takes either moon to
        /// radiance, so the two keep their relationship.
        osg::Vec3f mColour;
    };

    /// The two painted faces, in a scene's texture table, held rather than named by a material:
    /// the disc is drawn by a ray that reached nothing, so the sweep would take the slot back.
    struct MoonFaces
    {
        Index mMasser = sNoIndex;
        Index mSecunda = sNoIndex;

        Index of(Moon moon) const { return moon == Moon::Masser ? mMasser : mSecunda; }
    };

    /// Adds `tx_masser_full.dds` and `tx_secunda_full.dds` to `scene` and holds them there for the
    /// life of the scene. A moon drawn from the mean of its portrait is a coloured circle.
    MoonFaces addMoonFaces(SceneDesc& scene);

    /// A moon placed from angles `MWWorld::MoonModel` worked out. What a moon *is* once those
    /// angles are known — where its face points, how wide it is, which way its terminator falls —
    /// is one answer and lives here.
    ///
    /// @param alongArc degrees travelled from the horizon it rose at, zero to 180.
    /// @param axisOffset degrees the whole arc is swung about the zenith.
    /// @param phase which of the eight painted phases, counted from full.
    /// @param alpha the daylight fade, with the weather's `Glare_View` on it —
    ///        `Sky::MoonState::mDaylightFade`. Whether the moon is up at all is `alongArc`.
    MoonPlacement placeMoon(Moon moon, float alongArc, float axisOffset, Sky::MoonPhase phase, float alpha);

    /// A placement as the shader takes it — one conversion, so a moon read off the weather system
    /// and one worked out from a date reach the shader the same way.
    Shaders::MoonDisc describeMoon(const MoonPlacement& placement);

    /// The angular radius a placement gives that moon, in radians, out of the renderer the game
    /// already has: `Moons_<name>_Size` is scaled by 450/125 onto a quad of half-extent 0.5 a
    /// thousand units off (`gl/skyutil.cpp`), so the disc is `atan(1.8 * size / 1000)`. Masser's
    /// 94 comes to 9.6 degrees and Secunda's 40 to 4.1 — thirty-five times the sun.
    float moonAngularRadius(Moon moon);
}

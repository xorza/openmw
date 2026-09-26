#pragma once

#include <osg/Vec3f>

#include <components/sky/moonstate.hpp>
#include <components/vfs/pathutil.hpp>

#include "runs.hpp"
#include "shaders/sky.h"

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

        /// What this moon delivers to a surface facing it, linear, before `mPaint`: what lights
        /// anything is `getPaintedIrradiance`. Zero exactly where the fade is, which is the one
        /// test a shader makes before it spends a shadow ray; a new moon is three parts in ten
        /// thousand, which is what the photometry says.
        osg::Vec3f mIrradiance;

        /// What the air leaves of it, per channel — `Rtx::airTransmittance` at its own elevation,
        /// which is what lets a moon rise where the engine draws none under
        /// `Moons_<name>_Fade_End_Angle`. The light is dimmed by it too, on the host.
        osg::Vec3f mThroughAir{ 1.0f, 1.0f, 1.0f };

        /// The mean opaque texel of this moon's portrait, linear and unscaled — what the disc falls
        /// back to where no portrait is loaded. `Shaders::MOON_RADIANCE` takes either moon to
        /// radiance, so the two keep their relationship.
        osg::Vec3f mColour;

        /// What a script painted the face, linear: `Moons_Script_Color` while the game says
        /// Secunda is red, and white otherwise. Over the face and over what the moon delivers
        /// alike, because a moon that is drawn red lights red — the rasterizer's `moonBlend` paints
        /// the disc alone, and the disc is the only light it has.
        osg::Vec3f mPaint{ 1.0f, 1.0f, 1.0f };

        /// What the moon lights anything with: `mIrradiance` under `mPaint`. The one spelling, so
        /// the ground and the cloud deck are lit by the same moon.
        osg::Vec3f getPaintedIrradiance() const { return osg::componentMultiply(mIrradiance, mPaint); }
    };

    /// The two painted faces, in a scene's texture table, held rather than named by a material:
    /// the disc is drawn by a ray that reached nothing, so the sweep would take the slot back. And
    /// how wide each is drawn, which is fixed for the run and read with them.
    struct MoonFaces
    {
        Index mMasser = sNoIndex;
        Index mSecunda = sNoIndex;

        /// `moonAngularRadius` of each moon's `Moons_<name>_Size`, in radians.
        float mMasserRadius = 0.0f;
        float mSecundaRadius = 0.0f;

        Index of(Moon moon) const { return moon == Moon::Masser ? mMasser : mSecunda; }
        float radiusOf(Moon moon) const { return moon == Moon::Masser ? mMasserRadius : mSecundaRadius; }
    };

    /// Each moon's `Moons_<name>_Size`, as the configuration states it, which the host passes in.
    struct MoonSizes
    {
        float mMasser = 0.0f;
        float mSecunda = 0.0f;
    };

    /// The painted face of `moon`, as the content files name it: what `addMoonFaces` reads and what
    /// the game preloads, from this one answer.
    constexpr VFS::Path::NormalizedView moonFaceOf(const Moon moon)
    {
        return moon == Moon::Masser ? VFS::Path::NormalizedView("textures/tx_masser_full.dds")
                                    : VFS::Path::NormalizedView("textures/tx_secunda_full.dds");
    }

    /// Adds each moon's face, `moonFaceOf`, to `scene` and holds it there until `dropMoonFaces`,
    /// and how wide `sizes` draws each moon. A moon drawn from the mean of its portrait is a
    /// coloured circle. A moon of size nought is not drawn, as the game draws none; one whose size
    /// is below nought or not finite is refused to `scene`.
    MoonFaces addMoonFaces(SceneDesc& scene, const MoonSizes& sizes);

    /// Gives both holds back, so a scene the world has left holds nothing of its moons.
    void dropMoonFaces(SceneDesc& scene, const MoonFaces& faces);

    /// A moon placed from angles `MWWorld::MoonModel` worked out. What a moon *is* once those
    /// angles are known — where its face points, how wide it is, which way its terminator falls —
    /// is one answer and lives here.
    ///
    /// @param faces which portrait the moon wears and how wide it is. A moon of no width is not
    ///        drawn.
    /// @param alongArc degrees travelled from the horizon it rose at, zero to 180.
    /// @param axisOffset degrees the whole arc is swung about the zenith.
    /// @param phase which of the eight painted phases, counted from full.
    /// @param alpha the daylight fade, with the weather's `Glare_View` on it —
    ///        `Sky::MoonState::mDaylightFade`. Whether the moon is up at all is `alongArc`.
    MoonPlacement placeMoon(
        const MoonFaces& faces, Moon moon, float alongArc, float axisOffset, Sky::MoonPhase phase, float alpha);

    /// A placement as the shader takes it — one conversion, so a moon read off the weather system
    /// and one worked out from a date reach the shader the same way.
    Shaders::MoonDisc describeMoon(const MoonPlacement& placement);

    /// The angular radius of a moon of `Moons_<name>_Size` `size`, in radians, out of the renderer
    /// the game already has: the size is scaled by 450/125 onto a quad of half-extent 0.5 a
    /// thousand units off (`apps/openmw/mwrender/skyutil.cpp`), so the disc is `atan(1.8 * size /
    /// 1000)`. Masser's 94 comes to 9.6 degrees and Secunda's 40 to 4.1 — thirty-five times the
    /// sun. Nought for a size that is not a finite number above nought.
    float moonAngularRadius(float size);
}

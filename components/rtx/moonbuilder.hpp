#pragma once

#include <osg/Vec3f>

#include "scenedesc.hpp"
#include "shaders/visibility.h"

namespace Rtx
{
    /// Which of the two moons over Vvardenfell.
    enum class Moon
    {
        Masser,
        Secunda,
    };

    /// Where a moon stands, how big it is, and how much of it the sun has.
    ///
    /// **A disc rather than a body.** A ray that reaches nothing finds the moon the way it finds the
    /// sun — there is no sphere in any acceleration structure — so what a moon is here is a
    /// direction, an angular size, and the two axes its face is painted along.
    struct MoonPlacement
    {
        /// Unit vector toward the moon.
        osg::Vec3f mDirection;

        /// The face's own axes, unit and perpendicular to `mDirection` and to each other: `mRight`
        /// runs along the portrait's `u` and `mUp` against its `v`.
        ///
        /// **A moon is not a billboard.** It keeps its face toward the world and its orientation
        /// toward its own arc, so the portrait turns against the horizon as the moon crosses — which
        /// is what `Moon::setState` builds out of the same two rotations and what the painted maria
        /// need if they are not to slide.
        osg::Vec3f mRight;
        osg::Vec3f mUp;

        /// Half the angle the disc subtends, in radians.
        float mAngularRadius = 0.0f;

        /// How far round its cycle the moon is, in radians: **zero is full and pi is new**.
        ///
        /// The lit share of the face is `(1 + cos) / 2`, and the sign of the sine says which limb
        /// keeps it — so waxing and waning are one number rather than a flag beside it. The game
        /// ships eight painted phases and this is the angle each of them stands for.
        float mPhaseAngle = 0.0f;

        /// What the game fades the moon out by near the horizon and around the ends of its arc, from
        /// zero to one. Zero is a moon that is not there to be drawn.
        float mAlpha = 0.0f;

        /// The painted face in the scene's texture table, or `sNoIndex` for none.
        Index mFace = sNoIndex;

        /// The mean opaque texel of this moon's portrait, linear and unscaled.
        ///
        /// **What the disc falls back to where no portrait is loaded.** Masser is red and Secunda is
        /// grey and the red one is two and a half times the darker, which is a fact about the art;
        /// `Shaders::MOON_RADIANCE` is what takes either of them to radiance, so the two moons
        /// keep their relationship and the level stays in one place.
        osg::Vec3f mColour;
    };

    /// Where a moon stands on `day` at `hour`, out of the `Moons_*` settings.
    ///
    /// **`Sky::MoonModel`'s clock, reached from an hour rather than from a weather system.** The
    /// game asks that same component through `MWWorld::MoonModel` and hands the answer down as a
    /// `MoonState`; this asks it directly, because the harness has no weather system to ask. One
    /// arithmetic, two routes to it.
    ///
    /// @param day days since the world began, on Morrowind's own count: the game starts on day 0,
    ///        which the rise-hour formula anchors to 16 Last Seed.
    /// @param hour on a twenty-four hour clock.
    MoonPlacement makeMoon(Moon moon, int day, float hour);

    /// The two painted faces, in a scene's texture table.
    ///
    /// **Held rather than named by a material**, because a moon is not a surface anything stands on:
    /// the disc is drawn by a ray that reached nothing, so no material can speak for its texture and
    /// the sweep would take the slot back on the first frame a cell died.
    struct MoonFaces
    {
        Index mMasser = sNoIndex;
        Index mSecunda = sNoIndex;

        Index of(Moon moon) const { return moon == Moon::Masser ? mMasser : mSecunda; }
    };

    /// Adds `tx_masser_full.dds` and `tx_secunda_full.dds` to `scene` and holds them there.
    ///
    /// **One call and two callers**, as ever: the game's scene and the harness's both need the faces
    /// in the same table the trace reads, and a moon drawn from the mean of its portrait rather than
    /// the portrait itself is a coloured circle.
    ///
    /// Safe to call again on a scene that already has them — `addTexture` hands back the slot it
    /// already gave — though each call takes a hold, so each wants its own `dropMoonFaces`.
    MoonFaces addMoonFaces(SceneDesc& scene);

    /// Gives back the holds `addMoonFaces` took.
    void dropMoonFaces(SceneDesc& scene, const MoonFaces& faces);

    /// A moon placed from angles somebody else worked out.
    ///
    /// **Two callers reach the same sky by different routes.** The game runs a weather system and
    /// hands over the angles it settled on; the harness has none and derives them from the clock
    /// through `makeMoon`. What a moon *is* once those angles are known — where its face points, how
    /// wide it is, which way its terminator falls — is one answer and lives here.
    ///
    /// @param alongArc degrees travelled from the horizon it rose at, zero to 180.
    /// @param axisOffset degrees the whole arc is swung about the zenith.
    /// @param phase which of the eight painted phases, counted from full.
    /// @param alpha what the game fades it by. Zero is a moon that is not drawn.
    MoonPlacement placeMoon(Moon moon, float alongArc, float axisOffset, int phase, float alpha);

    /// A placement as the shader takes it.
    ///
    /// **One conversion and two callers**, which is the point it shares with `makeLight`: the game
    /// reads its moons off the weather system it already runs and the harness works them out from
    /// the clock, and a frame taken either way has to be under the same moons.
    Shaders::MoonDisc describeMoon(const MoonPlacement& placement);

    /// The angular radius `makeMoon` gives that moon, in radians.
    ///
    /// **Out of the renderer the game already has**, and not out of the mesh: `Moons_<name>_Size` is
    /// scaled by 450/125 onto a quad of half-extent 0.5 a thousand units off
    /// (`apps/openmw/mwrender/gl/skyutil.cpp:641`), so the disc is `atan(1.8 * size / 1000)` across
    /// its radius. Masser's 94 comes to 9.6 degrees and Secunda's 40 to 4.1 — a moon nineteen
    /// degrees wide, which is the sky Morrowind is remembered for and thirty-five times the sun.
    float moonAngularRadius(Moon moon);
}

#pragma once

#include <osg/Vec3f>

#include "scenedesc.hpp"
#include "shaders/look.h"
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

        /// What this moon delivers to a surface facing it, linear.
        ///
        /// **Its own colour at the level its own size and albedo give**, dimmed by how far round its
        /// cycle it is and by the fade the game applies. Zero exactly where the fade is — a moon
        /// that is down, or one the daylight has taken — which is the one test a shader makes
        /// before it spends a shadow ray. A new moon is not zero but three parts in ten thousand,
        /// which is what the photometry says it is worth.
        osg::Vec3f mIrradiance;

        /// What the air leaves of it, per channel — `Rtx::airTransmittance` at its own elevation.
        ///
        /// **This is what lets a moon rise.** The engine draws none under
        /// `Moons_<name>_Fade_End_Angle` and crossfades its face in over the twenty degrees above,
        /// which is a rasterizer keeping a lit quad off its own fogged dome. Here the air does that
        /// work and does it from the horizon up: a moon comes over the edge as a deep red ember and
        /// is itself by thirty degrees.
        ///
        /// **The light is dimmed by it too**, on the host and before it ever reaches a shader, so a
        /// moon on the horizon lights about as much as it shows.
        osg::Vec3f mThroughAir{ 1.0f, 1.0f, 1.0f };

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
    /// @param glare the weather's `Glare_View`, which fades the moons as it fades the stars — the
    ///        `Moon::adjustTransparency` the rasterizer applies after the weather has spoken.
    MoonPlacement makeMoon(Moon moon, int day, float hour, float glare);

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
    /// **Held for the life of the scene and never given back.** A moon is drawn by a ray that
    /// reached nothing, so no material can speak for the slot and the sweep would take it on the
    /// first frame a cell died. Both callers hold one scene for as long as they run.
    MoonFaces addMoonFaces(SceneDesc& scene);

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
    /// @param alpha the daylight fade, with the weather's `Glare_View` on it —
    ///        `MoonMoment::mDaylightFade`. What decides whether the moon is up at all is `alongArc`,
    ///        which the engine leaves at nought until it rises and returns to nought once it sets.
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

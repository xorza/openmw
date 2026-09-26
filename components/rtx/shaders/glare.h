#ifndef OPENMW_COMPONENTS_RTX_SHADERS_GLARE_H
#define OPENMW_COMPONENTS_RTX_SHADERS_GLARE_H

#include "hosttypes.h"
#include "portable.h"

// The sun glare fader: the wash of colour the game lays over the whole picture while the eye is
// turned toward a sun it can see.
//
// **The rasterizer's `SunGlareCallback`, in its own numbers.** The game states it as four:
// `Weather_Sun_Glare_Fader_Color`, doubled and clamped as the original did by setting one colour
// on two material terms; `_Max`, the most of the picture it may wash; `_Angle_Max`, the angle off
// the eye's axis at which it has faded to nothing; and the time-of-day fade the weather manager
// steps through dawn and dusk. Times the weather's `Glare_View`, and times how much of the sun's
// own quad the eye can see — an occlusion query over the quad's opaque texels against the world's
// depth, eased at ten per second toward what the query found. The trace has no query and no
// depth; it has the primary rays, and the ones inside the quad's disc say the same thing.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// Where `sunglare.comp` binds what it reads and writes in set 0, and how many there are. The
    /// shader's layout and the pass's own layout and writes are numbered by these and by nothing
    /// else, so the two cannot drift apart.
    const uint SUN_GLARE_BIND_COUNT = 0;
    const uint SUN_GLARE_BIND_SHARE = 1;
    const uint SUN_GLARE_BINDINGS = 2;

    /// The angular radius of the disc the query counts, in radians.
    ///
    /// **The sun's quad and not the sun's disc.** The query draws `tx_sun_05.dds` on the celestial
    /// body's quad — 450 units across, a thousand out — and keeps the texels whose alpha passes
    /// 0.8. Read off the file, the alpha crosses 0.8 seventeen texels out of sixty-four from the
    /// middle, so the disc is `atan(225 * 17 / 64 / 1000)`: three and a half degrees, where the
    /// sun the trace draws is a quarter of one.
    const float SUN_GLARE_QUERY_RADIUS = atan(225.0f * 17.0f / 64.0f / 1000.0f);

    /// How fast the seen share moves toward what the rays found, a second: the query's own
    /// easing, `dt * 10` a frame, which takes a tenth of a second from hidden to seen.
    const float SUN_GLARE_RATE = 10.0f;

    /// What the eye's rays count: how many fell inside the quad's disc, and how many of those met
    /// nothing that writes depth. Zeroed before the trace and read after it, on the device.
    struct SunGlareCount
    {
        uint mTotal;
        uint mSeen;
    };

    /// What the easing is told.
    struct SunGlareConstants
    {
        /// Seconds since the previous frame.
        float mElapsed;

        /// One where there is no previous share to move from: the share is taken outright.
        uint mReset;
    };

#ifdef RTX_HOST
    static_assert(sizeof(SunGlareCount) == 8, "SunGlareCount must be scalar-packed on every side");
    static_assert(sizeof(SunGlareConstants) == 8, "SunGlareConstants must be scalar-packed on every side");
}
#endif

#endif

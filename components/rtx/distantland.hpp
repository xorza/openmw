#pragma once

namespace Rtx
{
    /// How far from the eye the world is built, in units.
    ///
    /// **One number, and both the ground and the air are measured against it.** Rays go everywhere,
    /// so what this path needs is how much world exists — a property of the structure they are cast
    /// against, and not of a camera. `viewing distance` answers a different question for a renderer
    /// that culls, and at 7168 against a cell of 8192 it barely leaves the active grid.
    ///
    /// **The air has to follow it or none of this can be seen.** Fog extinction is a half-life
    /// measured in some distance; tuned to seven thousand units it swallows everything past the
    /// active grid, and a world built four cells out then looks exactly like one built none.
    float distantLandReach();
}

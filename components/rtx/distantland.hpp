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
    ///
    /// **And it has to follow it at the far end too, or the world is built for nothing.** The second
    /// element of the air — `Shaders::VisibilityConstants::mFogEdge` — closes at exactly this, and
    /// `QuadTreeWorld` culls a node on its distance to the eye, so the ball that is built and the
    /// ball that can be seen are the same one. Nothing is loaded past where the air has closed.
    float distantLandReach();
}

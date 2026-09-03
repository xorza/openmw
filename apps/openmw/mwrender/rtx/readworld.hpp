#pragma once

#include <optional>

#include <components/rtx/frameworld.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/skybuilder.hpp>

namespace MWRender
{
    struct WorldState;

    /// What one frame's world comes to, once the game's reading of it has been turned into the
    /// renderer's.
    struct WorldRead
    {
        Rtx::WorldReading mReading;

        /// A room's own exposure bias, and nothing for a frame under a sky.
        ///
        /// **A room is dark by the same measure a midnight is**, and holding one back by two stops
        /// is not what an eye walking into a room does — it adapts. `Rtx::makeRoomLight` is where
        /// the number is decided; a frame outdoors measures its own.
        std::optional<float> mExposureBias;
    };

    /// Turns what the game says about this frame's world into what the renderer builds a sky, an
    /// air and a sea out of.
    ///
    /// **Its own function because it is the whole of the translation and none of the frame.** What
    /// it does is decode colours, decide whether a cell has a sky, and hand every reading to the one
    /// builder that decides what a sun, a room light, an air and a moon may be — which is what keeps
    /// the game and the harness under the same sky. Nothing here touches a device, and nothing here
    /// is a decision this host makes on its own.
    ///
    /// @param sky what the scene holds of the dome, the clouds and the stars.
    /// @param faces the moon textures, which the placements point at.
    /// @param landReach how much world this renderer builds, in units. One reading for the whole
    ///        frame: the ground, the air and the distant lights are measured over the same number.
    /// @param seconds the world's clock, which the sea is animated by.
    WorldRead readWorld(const WorldState& world, const Rtx::SkyContent& sky, const Rtx::MoonFaces& faces,
        float landReach, float seconds);
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Rtx
{
    /// What a refusal is of, which also says what becomes of it: a texture is drawn as a stand-in,
    /// and everything else is left out.
    enum class Refused : std::uint8_t
    {
        /// A drawable's geometry, met by the walk.
        Mesh,

        /// A template the cell ring stands for the cells the game has not loaded.
        Model,

        Texture,

        /// A layer of the sky: the cloud cap, the star dome, or a weather's deck.
        SkyLayer,

        Moon,
        Lamp,

        /// A particle system, all of it.
        Emitter,

        /// Some of a particle system's sprites, the rest of it drawn.
        Sprites,
    };

    inline constexpr std::size_t sRefusedKinds = static_cast<std::size_t>(Refused::Sprites) + 1;

    /// One refusal, held by what made it until it can be reported: a reader thread hands its own to
    /// the frame with what it read, a describe that a read-only caller also runs hands them to the
    /// one that owns the scene, and a backend hands back what the device could not stand
    /// (`Renderer::getRefusals`).
    struct Refusal
    {
        Refused mKind = Refused::Mesh;
        std::string mName;
        std::string mWhy;
    };
}

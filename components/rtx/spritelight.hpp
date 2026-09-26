#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <components/vfs/pathutil.hpp>

namespace Rtx
{
    /// What a sprite's own texture leaves of the light crossing it, baked from its alpha: six-way
    /// lighting, because a billboard has no thickness and its alpha is the only record of where the
    /// thick parts are. For every texel and each of the four in-plane directions, how much light
    /// from that side reaches it through the texels between; the two out-of-plane directions are
    /// the alpha itself, so the shader derives them. A texel of alpha `a` seen through one sprite
    /// width has transmittance `1 - a`, so every texel a sideways ray passes multiplies by
    /// `(1 - a) ^ (1 / N)` for a level `N` texels across — no constant in it. Baked per level from
    /// that level's own alpha, because the shader samples both at one level. The channels are
    /// light from `+u`, `-u`, `+v`, `-v` in the texture's own coordinates, as `sprites.glsl` reads.
    ///
    /// **Made on the device as the sprite arrives**, `spritelight.comp`, and held by a test to the
    /// host's statement of it, `Testing::SpriteLightBake`. What the scene names a bake by is
    /// `keyFor`.
    class SpriteLightMap
    {
    public:
        /// The key a scene's baked-texture table holds for the bake of `source`.
        static std::string keyFor(VFS::Path::NormalizedView source);

        /// The source a key names, or nothing for a key that is some other bake's.
        static std::optional<VFS::Path::Normalized> sourceOf(std::string_view key);
    };
}

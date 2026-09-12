#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <components/vfs/pathutil.hpp>

#include "ownedtexture.hpp"
#include "texturedata.hpp"

namespace Rtx
{
    class AlphaImage;

    /// What a sprite's own texture leaves of the light crossing it, baked from its alpha: six-way
    /// lighting, because a billboard has no thickness and its alpha is the only record of where the
    /// thick parts are. For every texel and each of the four in-plane directions, how much light
    /// from that side reaches it through the texels between; the two out-of-plane directions are
    /// the alpha itself, so the shader derives them. A texel of alpha `a` seen through one sprite
    /// width has transmittance `1 - a`, so every texel a sideways ray passes multiplies by
    /// `(1 - a) ^ (1 / N)` for a level `N` texels across — no constant in it. Baked per level from
    /// that level's own alpha, because the shader samples both at one level. The channels are
    /// light from `+u`, `-u`, `+v`, `-v` in the texture's own coordinates, as `sprites.glsl` reads.
    class SpriteLightMap
    {
    public:
        /// The key a scene's baked-texture table holds for the bake of `source`.
        static std::string keyFor(VFS::Path::NormalizedView source);

        /// The source a key names, or nothing for a key that is some other bake's.
        static std::optional<VFS::Path::Normalized> sourceOf(std::string_view key);

        SpriteLightMap() = default;

        /// For a caller with one sprite to bake and no map to reuse. `build` is the whole of it.
        explicit SpriteLightMap(const AlphaImage& alpha) { build(alpha); }

        /// Bakes every level `alpha` carries. An alpha with none leaves this empty.
        ///
        /// **Refills this one rather than making another**, so a loader that bakes a cell's sprites
        /// keeps the room the last one grew. Whatever was here is gone, buffers apart.
        void build(const AlphaImage& alpha);

        bool isEmpty() const { return mTexture.isEmpty(); }

        /// The bake as a backend uploads it: linear, four bytes a texel, every level. Spans this
        /// object's own storage, so it must outlive the upload. Named for what it is and not for
        /// its source, because a view of a short string inside a moved object dangles.
        TextureData describe() const;

        /// One channel of one texel of one level, all of which must be inside the image.
        std::uint8_t at(std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t channel) const;

    private:
        OwnedTexture mTexture;
    };

}

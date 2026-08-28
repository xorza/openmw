#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <components/vfs/pathutil.hpp>

#include "texturedata.hpp"

namespace Rtx
{
    class AlphaImage;

    /// What a sprite's own texture leaves of the light crossing it, baked from its alpha.
    ///
    /// **A billboard has no thickness, and its alpha is the only record of where the thick parts
    /// are.** A puff lit as a flat card is one brightness from rim to rim however lumpy the blob the
    /// artist painted; a puff lit as a volume is dark where the sun has to cross the thick of it
    /// first and bright on the side the sun is on. That crossing is what is baked here: for every
    /// texel and each of the four directions in the sprite's own plane, how much light arriving
    /// from that side reaches it through the texels between. The two directions out of the plane
    /// need no bake — light from the front reaches the visible surface whole, and light from behind
    /// crosses the texel's own thickness, which is its alpha — so the shader derives them.
    ///
    /// **Six-way lighting, from the alpha rather than from a simulation.** The technique is the one
    /// authored effects bake out of a fluid solve; the game shipped no solve, only the alpha, so the
    /// density is what the alpha says. Each texel's optical density is the one that makes its own
    /// coverage right over a ball's width: a texel of alpha `a` seen through one sprite width has
    /// transmittance `1 - a`, and a ray crossing it sideways is inside it for one texel of that
    /// width, so every texel it passes multiplies by `(1 - a) ^ (1 / N)` for a level `N` texels
    /// across. There is no constant in that: an opaque texel stops the light outright and a blank
    /// one passes it.
    ///
    /// Baked per level from that level's own alpha, because the shader samples both at the same
    /// level and what it needs is the shadow of the alpha it sees.
    ///
    /// The four channels are, in order: light from `+u`, from `-u`, from `+v` and from `-v`, in the
    /// texture's own coordinates — so `+v` is the direction `v` grows in memory, whichever way the
    /// file's rows happen to be stored. `sprites.glsl` reads them in that order.
    class SpriteLightMap
    {
    public:
        /// The key a scene's baked-texture table holds for the bake of `source`.
        static std::string keyFor(VFS::Path::NormalizedView source);

        /// The source a key names, or nothing for a key that is some other bake's.
        static std::optional<VFS::Path::Normalized> sourceOf(std::string_view key);

        /// Bakes every level `alpha` carries. An alpha with none leaves this empty.
        explicit SpriteLightMap(const AlphaImage& alpha);

        bool isEmpty() const { return mLevels.empty(); }

        /// The bake as a backend uploads it: linear, four bytes a texel, every level. Spans this
        /// object's own storage, so it must outlive the upload.
        ///
        /// Named for what it is rather than for its source, because the name is a view and this
        /// object is moved about in a vector: a short source path would sit inside the string
        /// itself and the view would point at where it used to be.
        TextureData describe() const;

        /// One channel of one texel of one level, all of which must be inside the image.
        std::uint8_t at(std::uint32_t level, std::uint32_t x, std::uint32_t y, std::uint32_t channel) const;

    private:
        std::vector<MipLevel> mLevels;
        std::vector<std::uint8_t> mBytes;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
    };

}

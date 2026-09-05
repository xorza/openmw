#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "texturedata.hpp"

namespace Rtx
{
    /// The levels a texture needs, built where its file carried none.
    ///
    /// **Morrowind ships five thousand textures with a chain and a hundred and eighty-seven
    /// without**, and its rain is one of the hundred and eighty-seven: `tx_raindrop_01.dds` is eight
    /// by thirty-two with a single level. So every drop was read at its finest wherever it stood,
    /// and its peak alpha is 0.400 where the levels it lacks hold 0.283, 0.129 and 0.068 — a storm
    /// the rasterizer draws as a wash of grey came out as hard white marks that flickered as they
    /// fell. The rasterizer never had to say any of this: an `osg::Texture2D` asks the driver to
    /// generate what a file did not carry.
    ///
    /// **Decoded to loose texels rather than compressed again.** A block format cannot be filtered
    /// into another block without an encoder, and what wants this is a short list of small files —
    /// the largest in the game is five hundred and twelve square. So the whole texture is decoded
    /// once and uploaded uncompressed, and every texture that arrived with a chain keeps the bytes
    /// it arrived in.
    ///
    /// Owns its bytes, which is what separates it from a `TextureData`: that type is defined by
    /// spanning somebody else's, and there is nobody else's to span for a level no file holds.
    class MipChain
    {
    public:
        /// Builds a chain for a description carrying a single level, and nothing for one carrying
        /// more: Morrowind's own chains stop at eight texels rather than at one, and that last level
        /// is already the texture's mean to within what a ray can tell.
        explicit MipChain(const TextureData& described);

        /// Whether there was nothing to build, which is the ordinary case.
        bool isEmpty() const { return mLevels.empty(); }

        /// What was built, spanning this object's own storage. Its slot is the caller's to fill in,
        /// exactly as `describeImage`'s is.
        TextureData describe() const;

    private:
        std::vector<MipLevel> mLevels;

        /// Every level, back to back, four bytes a texel. The levels index into this by byte.
        std::vector<std::byte> mTexels;

        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        /// Whether what was decoded is display-encoded, which decides both what the filter averages
        /// in and which format the description names.
        bool mEncoded = true;

        /// The file this came from, spanning what the source description spanned.
        std::string_view mName;
    };
}

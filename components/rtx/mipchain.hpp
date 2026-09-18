#pragma once

#include "alphaimage.hpp"
#include "ownedtexture.hpp"
#include "texturedata.hpp"

namespace Rtx
{
    /// The levels a texture needs, built where its file carried none. Morrowind ships a hundred
    /// and eighty-seven such textures and its rain is one: read at its finest, a drop's peak alpha
    /// is 0.400 where the missing levels hold 0.283, 0.129 and 0.068, so a storm comes out as hard
    /// white marks that flicker. The rasterizer has the driver generate them. Decoded to loose
    /// texels rather than compressed again, because a block format cannot be filtered without an
    /// encoder and the largest of these files is five hundred and twelve square. Owns its bytes,
    /// unlike a `TextureData`, because there is nobody else's to span for a level no file holds.
    ///
    /// **The host's statement of the chain, which the game never runs.** The one the trace
    /// samples is made on the device as the texture arrives, `mipchain.comp`, and held to this
    /// one by a test.
    class MipChain
    {
    public:
        MipChain() = default;

        /// For a caller with one texture to build and no chain to reuse. `build` is the whole of it.
        explicit MipChain(const TextureData& described) { build(described); }

        /// Whether `described` is a file a chain is built for: one level, and more than a texel.
        /// Only a file that carried no chain at all — Morrowind's own stop short of a single
        /// texel, a 256-square texture ships six levels and ends at 8 by 8, and that last level
        /// is already the texture's own mean to within what a ray can tell; rebuilding those would
        /// decompress the whole game to gain nothing, and double what a cell's textures hold. A
        /// texel has no level below it, and a level with no extent has no texel to read. The one
        /// spelling of the rule, which the builder marks a description by and the device's pass
        /// answers to.
        static bool wantedFor(const TextureData& described);

        /// Builds a chain where `wantedFor` says, and nothing otherwise. Refills this one, so a
        /// loader keeps the room the last chain grew.
        void build(const TextureData& described);

        /// Empties the chain and keeps the room its texture grew.
        void reuse() { mTexture.reuse(); }

        /// Whether there was nothing to build, which is the ordinary case.
        bool isEmpty() const { return mTexture.isEmpty(); }

        /// What was built, spanning this object's own storage. Its slot is the caller's to fill in,
        /// exactly as `describeImage`'s is.
        TextureData describe() const;

    private:
        /// The finest level's alpha, read to weigh the colours by it. Not among what `build`
        /// resets, because nothing reads it but the line that fills it.
        AlphaImage mAlpha;

        /// Every level, back to back, four bytes a texel, with the name and the format beside them.
        OwnedTexture mTexture;

        /// Whether what was decoded is display-encoded, which decides what the filter averages in
        /// and which format the description names.
        bool mEncoded = true;
    };
}

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
    class MipChain
    {
    public:
        MipChain() = default;

        /// For a caller with one texture to build and no chain to reuse. `build` is the whole of it.
        explicit MipChain(const TextureData& described) { build(described); }

        /// Builds a chain for a description carrying a single level, and nothing for one carrying
        /// more: Morrowind's own chains stop at eight texels, which is already the mean to within
        /// what a ray can tell. Refills this one, so a loader keeps the room the last chain grew.
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

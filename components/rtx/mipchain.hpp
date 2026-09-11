#pragma once

#include "alphaimage.hpp"
#include "ownedtexture.hpp"
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
        MipChain() = default;

        /// For a caller with one texture to build and no chain to reuse. `build` is the whole of it.
        explicit MipChain(const TextureData& described) { build(described); }

        /// Builds a chain for a description carrying a single level, and nothing for one carrying
        /// more: Morrowind's own chains stop at eight texels rather than at one, and that last level
        /// is already the texture's mean to within what a ray can tell.
        ///
        /// **Refills this one rather than making another**, so a loader that describes a cell's
        /// worth keeps the room the last chain grew. Whatever was here is gone, buffers apart.
        void build(const TextureData& described);

        /// Empties the chain and keeps the room its texture grew.
        void reuse() { mTexture.reuse(); }

        /// Whether there was nothing to build, which is the ordinary case.
        bool isEmpty() const { return mTexture.isEmpty(); }

        /// What was built, spanning this object's own storage. Its slot is the caller's to fill in,
        /// exactly as `describeImage`'s is.
        TextureData describe() const;

    private:
        /// The finest level's alpha, read to weigh the colours by it. Held rather than made per
        /// build for the reason the texels are: a pool of these builds a cell's worth.
        ///
        /// **Deliberately not among what `build` resets**, because nothing reads it but the line
        /// that fills it — and emptying it is exactly the room a build is meant to keep.
        AlphaImage mAlpha;

        /// Every level, back to back, four bytes a texel, with the name and the format beside them.
        OwnedTexture mTexture;

        /// Whether what was decoded is display-encoded, which decides what the filter averages in
        /// and which format the description names.
        bool mEncoded = true;
    };
}

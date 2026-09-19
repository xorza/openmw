#ifndef OPENMW_COMPONENTS_RTX_SHADERS_DIGEST_H
#define OPENMW_COMPONENTS_RTX_SHADERS_DIGEST_H

#include "gbuffer.h"
#include "hosttypes.h"
#include "portable.h"

// A digest of the frame's images, taken on the device: what a comparison of two runs compares
// where the picture past the upscaler is the network's and not this renderer's. `digest.comp`
// folds every texel of every image into four words an image, a workgroup at a time, and the host
// folds the words of the texels the same way in a test, so the two are one function of an image's
// bits and neither trusts the other.
//
// **Four words, each a sum or an exclusive-or over the texels.** Both are commutative, so the
// order the workgroups land in — which is the device's and never the same twice — cannot reach the
// answer. Two independent mixes of each texel, kept apart as a sum and as an exclusive-or, are a
// hundred and twenty-eight bits a single changed bit anywhere in the image moves.

#ifdef RTX_HOST
namespace Rtx::Shaders
{
#endif

    /// One side of the square of texels a workgroup digests.
    const uint DIGEST_WORKGROUP = 16u;

    /// Words a digest holds per image: the sum and the exclusive-or of the first mix, then of the
    /// second.
    const uint DIGEST_LANES = 4u;

    /// What one frame digests: every channel of the trace, and the composite that folded them.
    const uint DIGEST_IMAGES = CHANNEL_COUNT + 1u;

    /// Which image the composite is, after the channels, which are their binding: image `i`'s
    /// words are the `DIGEST_LANES` from `i * DIGEST_LANES`.
    const uint DIGEST_COMPOSITE = CHANNEL_COUNT;

    /// The two seeds, one per mix, so the two lanes of a texel are not one word twice.
    const uint DIGEST_SEED_SUM = 0x9e3779b9u;
    const uint DIGEST_SEED_XOR = 0x85ebca6bu;

    /// One word folded into a running hash: MurmurHash3's block step, whose every operation is
    /// the same in both languages because it is unsigned and wraps.
    RTX_SHADER uint digestFold(uint hash, uint word)
    {
        word *= 0xcc9e2d51u;
        word = (word << 15u) | (word >> 17u);
        word *= 0x1b873593u;

        hash ^= word;
        hash = (hash << 13u) | (hash >> 19u);
        return hash * 5u + 0xe6546b64u;
    }

    /// The mix of one texel's four channels and where it stands. The position goes in so that two
    /// texels swapped read as a change, which a sum of their values alone would not see.
    RTX_SHADER uint digestTexel(uint r, uint g, uint b, uint a, uint x, uint y, uint seed)
    {
        uint hash = digestFold(seed, x);
        hash = digestFold(hash, y);
        hash = digestFold(hash, r);
        hash = digestFold(hash, g);
        hash = digestFold(hash, b);
        hash = digestFold(hash, a);

        // MurmurHash3's finaliser, so a texel that differs in one high bit moves every bit of the
        // word rather than a few.
        hash ^= hash >> 16u;
        hash *= 0x85ebca6bu;
        hash ^= hash >> 13u;
        hash *= 0xc2b2ae35u;
        hash ^= hash >> 16u;
        return hash;
    }

    /// What the dispatch is told: how far the images reach, which is one extent for all of them.
    struct DigestConstants
    {
        uint mWidth;
        uint mHeight;
    };

#ifdef RTX_HOST
    static_assert(sizeof(DigestConstants) == 8, "DigestConstants must be scalar-packed on every side");
}
#endif

#endif

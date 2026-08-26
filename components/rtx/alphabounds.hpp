#pragma once

#include <cstdint>
#include <vector>

namespace Rtx
{
    class AlphaImage;

    /// Which side of a cutoff something is on, in the encoding a four-state micromap element carries.
    ///
    /// **One enum for the mask's answer and the hardware's table**, because they are the same three
    /// values: what `AlphaBounds` decides about a patch is written into a micromap unchanged, and a
    /// second enum with a conversion between them would only be somewhere for the two to disagree.
    enum class Opacity : std::uint8_t
    {
        /// The hit is ignored outright, and traversal passes through without asking.
        Transparent = 0,

        /// The hit is taken, and traversal confirms it without asking.
        Opaque = 1,

        /// Traversal has to ask — which is what every candidate of every cutout does today.
        ///
        /// **The specification has two of these** — *unknown transparent* and *unknown opaque* — and
        /// they differ only under the two-state override, which is a ray flag and an instance flag
        /// this renderer sets neither of. One of them is the whole of what a ray here can see, and
        /// carrying both would be carrying a distinction nothing reads.
        Unknown = 2,
    };

    /// A patch of texture coordinates, as the corner-wise smallest and largest a sample can reach.
    struct UvBox
    {
        float mFromU = 0.0f;
        float mFromV = 0.0f;
        float mToU = 0.0f;
        float mToV = 0.0f;
    };

    /// Which side of a cutoff a patch of a mask is wholly on, whichever level the cone reads it at.
    ///
    /// **A micromap is a static table and the cutout it stands in for is not, and this is what
    /// closes the gap.** `alphaPasses` samples the mask through the mip chain, at whatever level the
    /// ray's cone can resolve — deliberately, because a mask point-sampled at its finest level is a
    /// coin toss per pixel and a canopy comes back as speckle that crawls. So a microtriangle marked
    /// opaque from the finest level alone would override the softening the cone was giving it, and
    /// the picture would change at distance.
    ///
    /// The way out is not a dilation wide enough to guess at: it is to bound the sample by *every*
    /// level of the chain at once. A trilinear read is a convex combination of texels drawn from two
    /// neighbouring levels, so it is bracketed by the smallest and largest texel either level could
    /// contribute — and a patch whose bracket lies wholly on one side of the cutoff answers the same
    /// way no matter how far away it is seen from. Everything else stays unknown and goes on asking,
    /// which is exactly what it should do.
    ///
    /// **The ring is derived rather than measured**, one level at a time. A bilinear read at level
    /// *l* reaches the four texels bracketing the point, which are inside the three-by-three around
    /// the level-*l* texel the point sits in — so each level contributes the minimum and the maximum
    /// of its own three-by-three, and the collapse of all of them down to the finest level's grid is
    /// what this holds.
    ///
    /// Built against one cutoff, because the counts below are of texels already compared against it.
    /// A texture worn by two materials with different cutoffs is two of these, which is the pairing
    /// a caller should key its cache on.
    class AlphaBounds
    {
    public:
        /// @param cutoff the alpha at or above which a texel is material rather than hole, in the
        ///        nought-to-one the shader compares against.
        AlphaBounds(const AlphaImage& mask, float cutoff);

        /// Whether there was nothing to read. A texture whose cutout could not be decoded has no
        /// verdict to give, and a caller must leave its geometry asking rather than answer for it.
        bool isEmpty() const { return mWidth == 0; }

        /// The finest level's extent, which is the grid a patch is measured against.
        std::uint32_t getWidth() const { return mWidth; }
        std::uint32_t getHeight() const { return mHeight; }

        /// What `patch` is wholly, or `Unknown` where it is not wholly anything. It may run outside
        /// the unit square, and wraps into it the way the one repeating sampler this scene shares
        /// does.
        Opacity classify(const UvBox& patch) const;

    private:
        /// How many texels of a half-open box pass, out of the two counts below.
        std::uint32_t countIn(const std::vector<std::uint32_t>& sums, std::uint32_t fromX, std::uint32_t fromY,
            std::uint32_t toX, std::uint32_t toY) const;

        /// The same, over a box that may wrap or wrap several times over.
        std::uint64_t countWrapped(const std::vector<std::uint32_t>& sums, std::int64_t fromX, std::int64_t fromY,
            std::uint32_t acrossX, std::uint32_t acrossY) const;

        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        /// Summed-area tables over the finest level's grid: how many texels of a box are certainly
        /// material, and how many are certainly hole.
        ///
        /// **Two counts and not a pair of minimum and maximum images**, because what a microtriangle
        /// asks is whether a whole box agrees, and a box query against a summed area is four reads
        /// however large the box is. Walking the texels instead is what the classifier does for
        /// every microtriangle of every triangle of a mesh, and a canopy card's triangles cover the
        /// same texels over and over.
        ///
        /// One row and one column wider than the image, the first of each being zero, so a box that
        /// starts at the edge needs no branch.
        std::vector<std::uint32_t> mMaterial;
        std::vector<std::uint32_t> mHole;
    };
}

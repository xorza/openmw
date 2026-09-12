#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>

namespace Rtx
{
    /// What a shape's triangles turned out to be, once its reversed twins were folded away. Two
    /// facts and neither follows from the other: a shape may be both — two exteriors hold one
    /// each — so `ShapeFold` reports them apart rather than as one kind.
    struct FoldedShape
    {
        /// Every triangle was one of a reversed pair, so the content doubled the whole shape for its
        /// back: a leaf, a fern, a grass card, a tabard. What that is for is `ShapeFold`'s doc, and
        /// a shader reads it as leave to light the mesh through its back.
        bool mSheet = false;

        /// Every edge of what survives carries a triangle each way, so the shape has no boundary and
        /// a ray that enters it leaves through the far side. Which of a surface's two normals is
        /// lying depends on this: a boulder's interpolated normals describe it and its facets do
        /// not, and an unbacked quad is the reverse — `litCosine` reads this to tell them apart.
        /// Matched on positions and not on indices, because Morrowind splits a vertex at every UV
        /// seam. Little of the game answers yes: a rock is modelled as a dome with no base.
        bool mClosed = false;
    };

    /// Folds the reversed twin every sheet in the game is doubled with back into one triangle, and
    /// says what the shape was. Morrowind has no two-sided flag, so the content draws a card's
    /// back by modelling it: a second triangle over the same three positions, wound the other way,
    /// with vertices of its own — 3670 shapes in the game are nothing but such pairs. This
    /// renderer culls nothing, so it meets both copies at the same depth and light passing through
    /// the card would be taken off twice. A shape that was nothing but pairs is a sheet, which is
    /// what lets a leaf carry the light that falls on its far side.
    class ShapeFold
    {
    public:
        /// Drops the second of every reversed pair, compacting `indices` in place, and says what
        /// the shape came to. Exact equality of positions, because the twin is a copy and not a
        /// remodel; a copy wound the same way is not a twin and is left. One pass and no allocation
        /// per triangle, because a cell crossing folds tens of thousands of triangles a second.
        FoldedShape fold(std::span<const osg::Vec3f> positions, std::vector<std::uint32_t>& indices);

    private:
        /// One triangle's corners, rotated so the least comes first: the winding survives and where
        /// the file happened to start the triangle does not, which is what lets two spellings of one
        /// triangle compare equal.
        struct Corners
        {
            osg::Vec3f mCorner[3];

            bool operator==(const Corners& other) const;

            /// Over the corner bits, with a zero normalised: -0 and 0 compare equal above, and a
            /// spelling that hashed the two apart would never find its twin.
            std::size_t hash() const;
        };

        enum class Fate : std::uint8_t
        {
            Alone,
            Kept,
            Dropped,
        };

        /// An empty table slot, and the end of a chain.
        static constexpr std::uint32_t sNoEntry = ~std::uint32_t{ 0 };

        /// One edge of what survived the fold, and how many triangles ran along it each way.
        struct Edge
        {
            osg::Vec3f mEnd[2];
            std::uint32_t mForward = 0;
            std::uint32_t mBackward = 0;
        };

        static Corners canonical(const osg::Vec3f& a, const osg::Vec3f& b, const osg::Vec3f& c);

        /// Whether every edge of `indices` carries one triangle each way. See `FoldedShape::mClosed`.
        /// Its own pass over three times as many entries as the fold, and its own buffers, because
        /// the fold's are still holding what the pairing wrote.
        bool closes(std::span<const osg::Vec3f> positions, std::span<const std::uint32_t> indices);

        /// The triangle a spelling is held under, or `sNoEntry`. Open addressed with linear
        /// probing, a power of two long and never more than half full, so a probe always ends. The
        /// slot holds the chain's head, because a chunk with four copies of a card pairs them two
        /// by two.
        std::vector<std::uint32_t> mTable;

        /// Each triangle's spelling and its hash, so a probe compares an index rather than
        /// recomputing corners it has already rotated once.
        std::vector<Corners> mSpelling;
        std::vector<std::size_t> mHashes;

        /// The next triangle sharing a spelling, indexed by triangle, ascending — so a pair keeps
        /// the copy the file wrote first. `sNoEntry` ends a chain.
        std::vector<std::uint32_t> mNext;

        std::vector<Fate> mFates;

        /// `closes`'s own table and edge list. A slot holds an index into `mEdges`.
        std::vector<std::uint32_t> mEdgeTable;
        std::vector<Edge> mEdges;
    };
}

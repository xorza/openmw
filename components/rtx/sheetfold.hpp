#ifndef OPENMW_COMPONENTS_RTX_SHEETFOLD_H
#define OPENMW_COMPONENTS_RTX_SHEETFOLD_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <osg/Vec3f>

namespace Rtx
{
    /// Folds the reversed twin every sheet in the game is doubled with back into one triangle.
    ///
    /// **Morrowind has no two-sided flag, and its leaves are two-sided anyway.** The rasterizer
    /// culls back faces for the whole scene and the three shipped archives hold no
    /// `NiStencilProperty` to say otherwise, so the content draws a card's back by modelling it: a
    /// second triangle over the same three positions, wound the other way, with vertices and
    /// normals of its own. 3670 shapes in the game are nothing but such pairs — every leaf, fern,
    /// grass card and tabard among them. A ray tracer that culls nothing meets both copies at the
    /// same depth, and light passing through the card would be taken off twice.
    ///
    /// So a pair becomes one triangle, and a shape that was nothing but pairs is a sheet: geometry
    /// the content meant to be seen and lit from either face, which is what lets a leaf carry the
    /// light that falls on its far side. Matched on position and not on index, because the twin
    /// has vertices of its own.
    class SheetFold
    {
    public:
        /// Drops the second of every reversed pair, compacting `indices` in place, and says whether
        /// every triangle was one of a pair.
        ///
        /// Exact equality of positions: the twin is a copy and not a remodel, and the content bears
        /// that out across every shape that has one. A copy wound the same way is not a twin — it
        /// says nothing about a back — and is left where it was.
        ///
        /// **One pass over the triangles, because a cell crossing is the cost this is on.** Every
        /// mesh a ring brings is folded as it arrives, a paged chunk is one merged geometry of every
        /// static in it, and a camera that moves changes chunk levels every few frames — so this ran
        /// on tens of thousands of triangles a second and was measured at an eighth of a streaming
        /// run's whole CPU time. A spelling is looked up rather than searched for.
        ///
        /// **And nothing here allocates per triangle**, which is why the table below is a flat array
        /// of indices rather than a node-based map. A map trades the sort this replaced for a node
        /// allocation and a free per triangle, and that is not a trade: written with
        /// `std::unordered_map` the island route spent 2.8 s reading its rings against the sort's
        /// 2.6, and 2.2 with the table below. Every buffer here is scratch the fold keeps and
        /// refills.
        bool fold(std::span<const osg::Vec3f> positions, std::vector<std::uint32_t>& indices);

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

        static constexpr std::uint32_t sNoTriangle = ~std::uint32_t{ 0 };

        static Corners canonical(const osg::Vec3f& a, const osg::Vec3f& b, const osg::Vec3f& c);

        /// The triangle a spelling is held under, or `sNoTriangle`. Open addressed with linear
        /// probing, a power of two long and never more than half full, so a probe always ends.
        ///
        /// **The slot holds the chain's head and not one triangle**, because a spelling can be
        /// written more than twice: a chunk with four copies of a card in it pairs them two by two.
        std::vector<std::uint32_t> mTable;

        /// Each triangle's spelling and its hash, so a probe compares an index rather than
        /// recomputing corners it has already rotated once.
        std::vector<Corners> mSpelling;
        std::vector<std::size_t> mHashes;

        /// The next triangle sharing a spelling, indexed by triangle, ascending — so a pair keeps
        /// the copy the file wrote first. `sNoTriangle` ends a chain.
        std::vector<std::uint32_t> mNext;

        std::vector<Fate> mFates;
    };
}

#endif

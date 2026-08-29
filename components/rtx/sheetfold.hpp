#ifndef OPENMW_COMPONENTS_RTX_SHEETFOLD_H
#define OPENMW_COMPONENTS_RTX_SHEETFOLD_H

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
        bool fold(std::span<const osg::Vec3f> positions, std::vector<std::uint32_t>& indices);

    private:
        struct Keyed
        {
            osg::Vec3f mCorner[3];
            std::uint32_t mTriangle;
        };

        enum class Fate : std::uint8_t
        {
            Alone,
            Kept,
            Dropped,
        };

        std::vector<Keyed> mKeys;
        std::vector<Fate> mFates;
    };
}

#endif

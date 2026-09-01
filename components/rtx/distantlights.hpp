#pragma once

#include <map>

#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/Vec4i>
#include <osg/ref_ptr>

#include <components/esm/refid.hpp>

#include "sceneextractor.hpp"

namespace osg
{
    class Group;
}

namespace Terrain
{
    class ObjectStorage;
}

namespace Rtx
{
    /// The lights of the cells the paging leaves dark.
    ///
    /// **`REC_LIGH` is not a paged type, and it must not become one.** `Terrain::pagedType` decides
    /// what a distant hillside is made of and both renderers read it, so a lantern four cells away
    /// has no model in either — that is upstream's picture and changing it would change the
    /// rasterizer's. What this fork cannot keep is the *light*: rays go everywhere, so a town that
    /// exists at dusk but casts nothing is the world stating something the content files do not.
    ///
    /// **A `Residency` and not a graph, for `TerrainResidency`'s reason.** What it builds is parented
    /// to nothing the rasterizer walks, handed to the mirror on the frame that asks, and gone from
    /// every other question anybody puts to the scene. The mirror then turns a `SceneUtil::LightSource`
    /// into a `Rtx::Light` exactly as it does for a lamp in the cell the player stands in — flicker,
    /// pulse and negative light included — so nothing here computes what a light is.
    ///
    /// **Outside the active grid and nowhere else.** Inside it the game has stood the real object,
    /// its light is on the graph, and the mirror has already found it; a second copy would be the
    /// same lantern counted twice.
    class DistantLights final : public Residency
    {
    public:
        DistantLights();
        ~DistantLights() override;

        DistantLights(const DistantLights&) = delete;
        DistantLights& operator=(const DistantLights&) = delete;

        /// What to read, and which worldspace to read of it. Null for a world with no storage, which
        /// `collect` answers by doing nothing.
        void follow(const Terrain::ObjectStorage* storage, ESM::RefId worldspace);

        /// Where the eye is, which decides which cells are near enough to matter.
        void setViewPoint(const osg::Vec3f& viewPoint) { mViewPoint = viewPoint; }

        /// How far out to read, in units — `distantLandReach`, which is what the ground is built to.
        ///
        /// **Told rather than asked, so this library needs no settings registry.** Nought reads no
        /// cell at all, which is what a host that never said leaves behind.
        void setReach(float units) { mReach = units; }

        /// The cells the game has stood for itself, as `Terrain::World` states them: minimum
        /// inclusive, maximum exclusive.
        void setActiveGrid(const osg::Vec4i& grid) { mActiveGrid = grid; }

        /// Whether there is a distant world for these to light.
        ///
        /// **False in an interior, where the eye's coordinates belong to another space.** The cells
        /// the reach would name around it are the exterior cells nearest the origin of a room, which
        /// stand nothing and mean nothing. What has been read stays read: a door is walked through
        /// both ways, and the reading is what this class exists to do once.
        void setOutdoors(bool outdoors) { mOutdoors = outdoors; }

        void collect(osg::NodeVisitor& visitor) override;

    private:
        /// The cell a world position stands in, on the exterior grid.
        static osg::Vec2i cellOf(const osg::Vec3f& position);

        /// Reads one cell's `LIGH` references and stands a light at each. Null where it holds none.
        osg::ref_ptr<osg::Group> build(const osg::Vec2i& cell) const;

        const Terrain::ObjectStorage* mStorage = nullptr;
        ESM::RefId mWorldspace;

        osg::Vec3f mViewPoint;
        osg::Vec4i mActiveGrid;
        float mReach = 0.0f;
        bool mOutdoors = true;

        /// Every cell read so far, by grid position, holding null where the cell stands no light.
        ///
        /// **Content, so a cell is read once for the life of the world.** What a `LIGH` reference
        /// says does not change with the hour or the weather — the flicker is a function of the
        /// simulation time and the mirror applies it every frame — so a cell that has been read
        /// costs a pointer to hand over and nothing else. The null entries are kept for the same
        /// reason: the absence is the answer, and reading the blocks again to find it out is the
        /// cost this avoids.
        ///
        /// **Never emptied**: a world that changed is a new `follow`, and that is what clears it.
        std::map<osg::Vec2i, osg::ref_ptr<osg::Group>> mCells;
    };
}

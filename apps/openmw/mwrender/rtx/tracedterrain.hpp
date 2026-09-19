#pragma once

#include <map>
#include <utility>
#include <vector>

#include <osg/Array>
#include <osg/ref_ptr>

#include <components/esm/refid.hpp>
#include <components/terrain/buffercache.hpp>
#include <components/terrain/world.hpp>

namespace osg
{
    class Geometry;
    class Group;
    class PositionAttitudeTransform;
}

namespace Terrain
{
    class Storage;
    class View;
}

namespace MWRender
{
    /// The ground as the ray tracer stands it: a `Terrain::World` that holds the storage, the
    /// worldspace and the active grid, and draws no chunks — `Rtx::CellRing` reads the land
    /// records itself, and a chunk the game built beside it would be one nothing traces.
    ///
    /// **A world that says it has no chunks**, rather than one the game finds out about: the
    /// preloader asks every world for a view to fill, and the borders a console command toggles
    /// are chunk geometry. Both are answered here, so upstream's callers run unchanged.
    ///
    /// **And a grid per loaded cell for the intersector**, under `Mask_Terrain` and nothing else.
    /// `RenderingManager::castRay` walks the scene graph, so `terrain obstructs focus`, dropping
    /// an object on the ground and Lua's `castRenderingRay` all answer off whatever stands there:
    /// the storage's heights under the rasterizer's own triangles, with no material, no texture
    /// and no normal, which a ray asks nothing of. The mirror's walk leaves `Mask_Terrain` out, so
    /// the trace never meets one.
    class TracedTerrain final : public Terrain::World
    {
    public:
        TracedTerrain(osg::Group& sceneRoot, Terrain::Storage& storage, unsigned int nodeMask, ESM::RefId worldspace);

        /// Out of line, where the grids' types are whole.
        ~TracedTerrain() override;

        /// A view that holds nothing and resets to nothing, because there is nothing to preload
        /// into it. The caller owns it, as `Terrain::World::createView` promises.
        Terrain::View* createView() override;

        /// Stands the cell's grid for the intersector, out of a grid a cell that left gave back
        /// where there is one: the arrays are refilled in place, so a cell arriving while the game
        /// runs allocates nothing after the first few.
        void loadCell(int x, int y) override;

        /// Takes the cell's grid down and keeps it for the next cell to arrive.
        void unloadCell(int x, int y) override;

        /// No chunks, so no borders: the command that toggles them is told they stayed off.
        void setBordersVisible(bool visible) override {}
        bool getBordersVisible() override { return false; }

    private:
        /// One cell's grid: the transform at the cell's middle, and the geometry under it whose
        /// positions are the storage's.
        struct CellGrid
        {
            osg::ref_ptr<osg::PositionAttitudeTransform> mRoot;
            osg::ref_ptr<osg::Geometry> mGeometry;
            osg::ref_ptr<osg::Vec3Array> mPositions;
        };

        /// A grid to stand a cell on: one a cell gave back, or a new one.
        CellGrid takeGrid();

        /// The rasterizer's own triangles over a cell's vertices, shared by every grid.
        Terrain::BufferCache mBuffers;

        /// What the storage writes beside the positions and a ray never reads, refilled per cell.
        osg::ref_ptr<osg::Vec3Array> mNormals;
        osg::ref_ptr<osg::Vec4ubArray> mColours;

        std::map<std::pair<int, int>, CellGrid> mCells;
        std::vector<CellGrid> mSpare;
    };
}

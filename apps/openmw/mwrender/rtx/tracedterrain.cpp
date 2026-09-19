#include "tracedterrain.hpp"

#include <utility>

#include <osg/Geometry>
#include <osg/PositionAttitudeTransform>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/terrain/storage.hpp>
#include <components/terrain/view.hpp>

namespace MWRender
{
    namespace
    {
        class NullView final : public Terrain::View
        {
        public:
            void reset() override {}
        };
    }

    TracedTerrain::TracedTerrain(
        osg::Group& sceneRoot, Terrain::Storage& storage, const unsigned int nodeMask, const ESM::RefId worldspace)
        : Terrain::World(&sceneRoot, &storage, nodeMask, worldspace)
        , mNormals(new osg::Vec3Array)
        , mColours(new osg::Vec4ubArray)
    {
    }

    TracedTerrain::~TracedTerrain() = default;

    Terrain::View* TracedTerrain::createView()
    {
        return new NullView;
    }

    TracedTerrain::CellGrid TracedTerrain::takeGrid()
    {
        if (!mSpare.empty())
        {
            CellGrid grid = std::move(mSpare.back());
            mSpare.pop_back();
            return grid;
        }

        CellGrid grid{
            .mRoot = new osg::PositionAttitudeTransform,
            .mGeometry = new osg::Geometry,
            .mPositions = new osg::Vec3Array,
        };

        // The triangles `TerrainGrid` draws a cell with, and no stitching flags: the vertex order
        // is `fillVertexBuffers`' own, and the diamond splits each quad the way the game does, so
        // a slope is cut here as it is cut on the screen.
        const auto verts = static_cast<unsigned int>(mStorage->getCellVertices(mWorldspace));
        grid.mGeometry->setUseDisplayList(false);
        grid.mGeometry->setVertexArray(grid.mPositions);
        grid.mGeometry->addPrimitiveSet(mBuffers.getIndexBuffer(verts, 0));
        grid.mRoot->addChild(grid.mGeometry);

        return grid;
    }

    void TracedTerrain::loadCell(const int x, const int y)
    {
        const std::pair<int, int> key(x, y);
        if (mCells.contains(key))
            return;

        Terrain::World::loadCell(x, y);

        // The whole cell at full detail, as the ring reads it: positions about the cell's middle,
        // which is where the transform stands them. A cell with no land record is the default
        // plane, which is the ground the rasterizer's chunk stands there too.
        CellGrid grid = takeGrid();
        const osg::Vec2f centre(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
        mStorage->fillVertexBuffers(0, 1.0f, centre, mWorldspace, *grid.mPositions, *mNormals, *mColours);
        grid.mPositions->dirty();
        grid.mGeometry->dirtyBound();

        const float cellSize = mStorage->getCellWorldSize(mWorldspace);
        grid.mRoot->setPosition(osg::Vec3f(centre.x() * cellSize, centre.y() * cellSize, 0.0f));

        mTerrainRoot->addChild(grid.mRoot);
        mCells.emplace(key, std::move(grid));
    }

    void TracedTerrain::unloadCell(const int x, const int y)
    {
        const auto found = mCells.find(std::pair<int, int>(x, y));
        if (found == mCells.end())
            return;

        Terrain::World::unloadCell(x, y);

        mTerrainRoot->removeChild(found->second.mRoot);
        mSpare.push_back(std::move(found->second));
        mCells.erase(found);
    }
}

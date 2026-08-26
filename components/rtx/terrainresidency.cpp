#include "terrainresidency.hpp"

#include <components/terrain/view.hpp>
#include <components/terrain/world.hpp>

namespace Rtx
{
    TerrainResidency::TerrainResidency() = default;

    TerrainResidency::~TerrainResidency() = default;

    void TerrainResidency::follow(Terrain::World* terrain)
    {
        if (mTerrain == terrain)
            return;

        mTerrain = terrain;
        mView = terrain == nullptr ? nullptr : terrain->createView();
    }

    void TerrainResidency::collect(osg::NodeVisitor& visitor)
    {
        if (mTerrain == nullptr || mView == nullptr)
            return;

        mTerrain->collect(mView.get(), mViewPoint, visitor);
    }
}

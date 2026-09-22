#include "tracedground.hpp"

#include "../../mwworld/ptr.hpp"
#include "rtxrenderer.hpp"

namespace MWRender
{
    TracedGround::TracedGround(osg::Group& sceneRoot, Terrain::Storage& storage, const unsigned int nodeMask,
        const ESM::RefId worldspace, RtxRenderer& renderer)
        : mTerrain(sceneRoot, storage, nodeMask, worldspace)
        , mRenderer(renderer)
    {
    }

    bool TracedGround::enableReference(int type, const MWWorld::ConstPtr& ptr, const bool enabled)
    {
        mRenderer.setReferenceEnabled(ptr.getCellRef().getRefNum(), enabled);
        return false;
    }

    bool TracedGround::blacklistReference(int type, const MWWorld::ConstPtr& ptr)
    {
        mRenderer.blacklistReference(ptr.getCellRef().getRefNum());
        return false;
    }

    void TracedGround::clear()
    {
        mRenderer.forgetReferences();
    }
}

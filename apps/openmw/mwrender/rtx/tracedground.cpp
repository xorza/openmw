#include "tracedground.hpp"

#include "../../mwworld/ptr.hpp"
#include "worldmirror.hpp"

namespace MWRender
{
    TracedGround::TracedGround(osg::Group& sceneRoot, Terrain::Storage& storage, const unsigned int nodeMask,
        const ESM::RefId worldspace, WorldMirror& mirror)
        : mTerrain(sceneRoot, storage, nodeMask, worldspace)
        , mMirror(mirror)
    {
    }

    bool TracedGround::enableReference(int type, const MWWorld::ConstPtr& ptr, const bool enabled)
    {
        mMirror.setReferenceEnabled(ptr.getCellRef().getRefNum(), enabled);
        return false;
    }

    bool TracedGround::blacklistReference(int type, const MWWorld::ConstPtr& ptr)
    {
        mMirror.blacklistReference(ptr.getCellRef().getRefNum());
        return false;
    }

    void TracedGround::clear()
    {
        mMirror.forgetReferences();
    }
}

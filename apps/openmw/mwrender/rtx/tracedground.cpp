#include "tracedground.hpp"

#include "rtxrenderer.hpp"

namespace MWRender
{
    TracedGround::TracedGround(osg::Group& sceneRoot, Terrain::Storage& storage, const unsigned int nodeMask,
        const ESM::RefId worldspace, RtxRenderer& renderer)
        : mTerrain(sceneRoot, storage, nodeMask, worldspace)
        , mRenderer(renderer)
    {
    }

    bool TracedGround::enableReference(
        int type, const ESM::RefNum refnum, const osg::Vec3f& position, const osg::Vec2i& cell, const bool enabled)
    {
        mRenderer.setReferenceEnabled(refnum, enabled);
        return false;
    }

    bool TracedGround::blacklistReference(
        int type, const ESM::RefNum refnum, const osg::Vec3f& position, const osg::Vec2i& cell)
    {
        mRenderer.blacklistReference(refnum);
        return false;
    }

    void TracedGround::clear()
    {
        mRenderer.forgetReferences();
    }
}

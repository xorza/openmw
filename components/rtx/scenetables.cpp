#include "scenetables.hpp"

#include <algorithm>

#include "material.hpp"
#include "meshinstance.hpp"

namespace Rtx
{
    std::span<const Shaders::GpuBone> SceneTables::getMeshBones(const Index mesh) const
    {
        return mDeformers.getMeshBones(mMeshes.getRows()[mesh]);
    }

    std::span<const float> SceneTables::getMeshWeights(const Index mesh) const
    {
        return mDeformers.getMeshWeights(mMeshes.getRows()[mesh]);
    }

    template <class Visit>
    void SceneTables::forEachPlacement(Visit&& visit) const
    {
        for (const MeshInstance& instance : mPlacements.getAll())
        {
            if (!instance.isPlaced())
                continue;

            // **Each mesh's own box carried through its instances**, rather than every vertex of
            // every instance — the difference between eight transforms per instance and several
            // hundred. The mesh kept it as its vertices arrived, so nothing is walked here at all.
            const osg::BoundingBoxf& box = mMeshes.getRows()[instance.mMesh].mBounds;
            if (!box.valid())
                continue;

            osg::BoundingBoxf placed;
            for (unsigned int corner = 0; corner < 8; ++corner)
                placed.expandBy(box.corner(corner) * instance.mTransform);

            visit(instance, placed);
        }
    }

    osg::BoundingBoxf SceneTables::getBounds() const
    {
        osg::BoundingBoxf bounds;
        forEachPlacement([&](const MeshInstance&, const osg::BoundingBoxf& box) { bounds.expandBy(box); });

        return bounds;
    }

    osg::BoundingBoxf SceneTables::getContentBoundsWithin(const osg::BoundingBoxf& region) const
    {
        osg::BoundingBoxf bounds;
        forEachPlacement([&](const MeshInstance& instance, const osg::BoundingBoxf& box) {
            // An instance with no material is not a backdrop — the untextured test scenes place
            // those, and a caller framing one means to see it.
            if (instance.mMaterial != sNoIndex && mMaterials.getRows()[instance.mMaterial].mKind == MaterialKind::Water)
                return;

            if (!box.intersects(region))
                return;

            // The part inside, so a chunk straddling the edge contributes where it overlaps rather
            // than dragging the answer out by its whole width.
            bounds.expandBy(osg::BoundingBoxf(std::max(box.xMin(), region.xMin()), std::max(box.yMin(), region.yMin()),
                std::max(box.zMin(), region.zMin()), std::min(box.xMax(), region.xMax()),
                std::min(box.yMax(), region.yMax()), std::min(box.zMax(), region.zMax())));
        });

        return bounds;
    }
}

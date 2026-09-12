#include "scenedesc.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <tuple>

namespace Rtx
{
    Index SceneDesc::addMesh(const MeshArrays& arrays, FoldedShape shape, Deform deform, Index deformer, Index material)
    {
        assert((material == sNoIndex || material < mMaterials.size()) && "a mesh wearing a material the scene lacks");

        return mMeshes.add(arrays, shape, deform, deformer, material);
    }

    void SceneDesc::poseRig(Index mesh, std::span<const Shaders::GpuBone> bones, const osg::BoundingBoxf& bounds)
    {
        assert(mesh < mMeshes.size());
        if (mDeformers.poseRig(mMeshes.getRows()[mesh], bones))
            mMeshes.notePosed(mesh, bounds);
    }

    void SceneDesc::poseMorph(Index mesh, std::span<const float> weights, const osg::BoundingBoxf& bounds)
    {
        assert(mesh < mMeshes.size());
        if (mDeformers.poseMorph(mMeshes.getRows()[mesh], weights))
            mMeshes.notePosed(mesh, bounds);
    }

    void SceneDesc::setMaterial(Index material, const Material& what)
    {
        if (!mMaterials.set(material, what))
            return;

        // Linear over the placements on the frame a surface crosses opaque, which a fade does twice
        // in its life; the flipbooks that animate every frame never come here.
        const std::span<const MeshInstance> placed = mPlacements.getAll();
        for (Index slot = 0; slot < placed.size(); ++slot)
            if (placed[slot].isPlaced() && placed[slot].mMaterial == material)
                mPlacements.rewrite(slot);
    }

    bool SceneDesc::hasDroppedHolds() const
    {
        return mMeshes.hasDroppedHolds() || mMaterials.hasDroppedHolds();
    }

    void SceneDesc::addLight(const Light& light)
    {
        mLights.push_back(light);
    }

    void SceneDesc::addEmitter(
        std::span<const Sprite> sprites, Index texture, bool additive, float width, Index lighting)
    {
        if (sprites.empty())
            return;

        // A quad's reach is its own diagonal: a rain streak ten times as tall as it is wide would
        // be cut off by a sphere measured on the width.
        const auto spanOf = [width](const Sprite& sprite) {
            assert((width > 0.0f) == (sprite.mAxis.length2() > 0.0f)
                && "an emitter and its sprites disagree about whether the quads hang in the world");

            return width > 0.0f ? std::sqrt(sprite.mAxis.length2() + width * width) : 1.0f;
        };

        // The centre of the sprites' own bounding box rather than their mean, and the reach measured
        // off it: a plume is a handful of parcels strung along one axis, and a mean sits where most
        // of them happen to be at this instant rather than where the extent is.
        osg::BoundingBoxf box;
        for (const Sprite& sprite : sprites)
        {
            const float rim = sprite.mRadius * spanOf(sprite);
            box.expandBy(sprite.mPosition - osg::Vec3f(rim, rim, rim));
            box.expandBy(sprite.mPosition + osg::Vec3f(rim, rim, rim));
        }

        const osg::Vec3f centre = box.center();
        float reach = 0.0f;
        for (const Sprite& sprite : sprites)
            reach = std::max(reach, (sprite.mPosition - centre).length() + sprite.mRadius * spanOf(sprite));

        mEmitters.push_back(SpriteEmitter{
            .mCentre = centre,
            .mReach = reach,
            .mSprites
            = Run{ .mOffset = static_cast<Index>(mSprites.size()), .mCount = static_cast<Index>(sprites.size()) },
            .mTexture = texture,
            .mLighting = lighting,
            .mAdditive = additive,
            .mWidth = width,
        });

        mSprites.insert(mSprites.end(), sprites.begin(), sprites.end());
    }

    Index SceneDesc::addInstance(const MeshInstance& instance)
    {
        assert(instance.mMesh < mMeshes.size());
        assert(instance.mMaterial == sNoIndex || instance.mMaterial < mMaterials.size());

        return mPlacements.add(instance);
    }

    void SceneDesc::orderLights()
    {
        // A total order, so that two lights the walk could hand over either way round come out the
        // same way round every time. Tied and not built, because a tuple of references copies
        // nothing over thousands of comparisons.
        std::sort(mLights.begin(), mLights.end(), [](const Light& a, const Light& b) {
            return std::tie(a.mPosition, a.mIntensity, a.mReach, a.mSourceRadius, a.mClearance)
                < std::tie(b.mPosition, b.mIntensity, b.mReach, b.mSourceRadius, b.mClearance);
        });
    }

    void SceneDesc::clearPlacement()
    {
        mLights.clear();

        mMeshes.clearDeformed();
        mSprites.clear();
        mEmitters.clear();
    }

    bool SceneDesc::release(std::span<const Index> meshes, std::span<const Index> materials)
    {
        // Only meshes and materials are asked, and that is now the whole of what this frees: a
        // texture goes when the last material or hold naming it lets go, wherever that happens.
        const std::size_t keptMeshes = mMeshes.mark(meshes);
        const std::size_t keptMaterials = mMaterials.mark(materials);

        // The ordinary frame leaves here. Asked of the marks and not of the span's length, which
        // agree only while the keep set names each survivor once.
        if (keptMeshes == mMeshes.getLiveCount() && keptMaterials == mMaterials.getLiveCount())
            return false;

        const std::size_t freedMeshes = mMeshes.sweep();
        const std::size_t freedMaterials = mMaterials.sweep();

        // The per-frame lists are left as the walk left them: the walk that would refill them is
        // the next frame's, and nothing in them can be stale after a walk of the whole world.
        // Neither is a structure change nor a shading change: the structures still describe
        // geometry nothing stands on, and the top level a frame rebuilds anyway stops them being
        // traced.
        return freedMeshes > 0 || freedMaterials > 0;
    }

    void SceneDesc::clearArrivals()
    {
        mMeshes.clearArrivals();
        mTextures.clearArrivals();
        mMaterials.clearArrivals();
        mDeformers.clearArrivals();
    }

    std::span<const Shaders::GpuBone> SceneDesc::getMeshBones(const Index mesh) const
    {
        return mDeformers.getMeshBones(mMeshes.getRows()[mesh]);
    }

    std::span<const float> SceneDesc::getMeshWeights(const Index mesh) const
    {
        return mDeformers.getMeshWeights(mMeshes.getRows()[mesh]);
    }

    template <class Visit>
    void SceneDesc::forEachPlacement(Visit&& visit) const
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

    osg::BoundingBoxf SceneDesc::getBounds() const
    {
        osg::BoundingBoxf bounds;
        forEachPlacement([&](const MeshInstance&, const osg::BoundingBoxf& box) { bounds.expandBy(box); });

        return bounds;
    }

    osg::BoundingBoxf SceneDesc::getContentBoundsWithin(const osg::BoundingBoxf& region) const
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

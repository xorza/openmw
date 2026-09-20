#include "scenedesc.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <string>
#include <tuple>

#include <osg/Vec3f>

#include "error.hpp"

namespace Rtx
{
    namespace
    {
        /// Atomic, because a picture's description is built wherever a view is made and the
        /// tests build theirs on any thread.
        std::atomic<std::uint64_t> sIdentities{ 0 };
    }

    SceneDesc::SceneDesc()
        : mIdentity(++sIdentities)
    {
    }

    Index SceneDesc::addMesh(const MeshArrays& arrays, FoldedShape shape, Deform deform, Index deformer)
    {
        return mMeshes.add(mDeformers, arrays, shape, deform, deformer);
    }

    void SceneDesc::checkPoses(const Index posed, const MeshArrays& arrays)
    {
        if (posed != arrays.mPositions.size())
            throw Error("a deforming mesh of " + std::to_string(arrays.mPositions.size())
                + " vertices on a rig or morph of " + std::to_string(posed));
    }

    DeformedMesh SceneDesc::addMesh(const MeshArrays& arrays, const FoldedShape shape, const RigSpec& rig)
    {
        checkPoses(rig.getVertexCount(), arrays);
        MeshTable::checkFits(arrays);

        const Index deformer = mDeformers.addRig(rig);
        return DeformedMesh{
            .mMesh = mMeshes.add(mDeformers, arrays, shape, Deform::Rig, deformer),
            .mDeformer = deformer,
        };
    }

    DeformedMesh SceneDesc::addMesh(const MeshArrays& arrays, const FoldedShape shape, const MorphSpec& morph)
    {
        checkPoses(morph.getVertexCount(), arrays);
        MeshTable::checkFits(arrays);

        const Index deformer = mDeformers.addMorph(morph);
        return DeformedMesh{
            .mMesh = mMeshes.add(mDeformers, arrays, shape, Deform::Morph, deformer),
            .mDeformer = deformer,
        };
    }

    Index SceneDesc::addMaterial(const Material& material)
    {
        return mMaterials.add(mTextures, material);
    }

    void SceneDesc::pose(Index mesh, std::span<const PoseWord> words, const osg::BoundingBoxf& bounds)
    {
        assert(mesh < mMeshes.size());
        if (mDeformers.pose(mMeshes.getRows()[mesh], words))
            mMeshes.notePosed(mesh, bounds);
    }

    void SceneDesc::setMaterial(Index material, const Material& what)
    {
        if (!mMaterials.set(mTextures, material, what))
            return;

        mPlacements.rewriteWearing(material, what.getTraversed());
    }

    bool SceneDesc::hasDroppedHolds() const
    {
        return mMeshes.hasDroppedHolds() || mMaterials.hasDroppedHolds();
    }

    void SceneDesc::addLight(const Light& light)
    {
        mTurn.expect(Turn::Open, Turn::Walked);
        mLights.push_back(light);
    }

    void SceneDesc::addEmitter(
        std::span<const Sprite> sprites, Index texture, bool additive, float width, Index lighting, bool falls)
    {
        mTurn.expect(Turn::Open, Turn::Walked);
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

        // Each sprite names the emitter that placed it as it is copied in: this is the one place
        // that knows, and a tile's list is sprites that have to say when their run changed.
        const auto emitter = static_cast<Index>(mEmitters.size());
        mEmitters.push_back(SpriteEmitter{
            .mCentre = centre,
            .mReach = reach,
            .mFirst = static_cast<Index>(mSprites.size()),
            .mCount = static_cast<Index>(sprites.size()),
            .mTexture = texture,
            .mFlags = (additive ? Shaders::EMITTER_ADDITIVE : 0u) | (falls ? Shaders::EMITTER_FALLS : 0u),
            .mWidth = width,
            .mLighting = lighting,
        });

        for (const Sprite& sprite : sprites)
        {
            Sprite& placed = mSprites.emplace_back(sprite);
            placed.mEmitter = emitter;
        }
    }

    Index SceneDesc::addInstance(const MeshInstance& instance)
    {
        assert(
            instance.mMesh < mMeshes.size() && mMeshes.isLive(instance.mMesh) && "a placement of a mesh nothing holds");
        assert((instance.mMaterial == sNoIndex || mMaterials.isLive(instance.mMaterial))
            && "a placement wearing a material nothing holds");

        // A plain opaque surface where the instance carries no material, which the untextured
        // test scenes place.
        const Material::Traversed worn = instance.mMaterial == sNoIndex
            ? Material::Traversed{}
            : mMaterials.getRows()[instance.mMaterial].getTraversed();

        return mPlacements.add(instance, worn);
    }

    bool SceneDesc::placementsStandOnLiveRows() const
    {
        for (const PlacementRow& row : mPlacements.getRows())
        {
            const MeshInstance& placed = row.mInstance;
            if (!placed.isPlaced())
                continue;

            if (!mMeshes.isLive(placed.mMesh))
                return false;
            if (placed.mMaterial != sNoIndex && !mMaterials.isLive(placed.mMaterial))
                return false;
        }

        return true;
    }

    bool SceneDesc::isConsistent() const
    {
        if (!placementsStandOnLiveRows())
            return false;

        for (Index slot = 0; slot < mTextures.getRows().size(); ++slot)
            if (!mTextures.isFree(slot) && mTextures.getHolds(slot) == 0)
                return false;

        for (Index slot = 0; slot < mDeformers.getDeformers().size(); ++slot)
            if (mDeformers.isLive(slot) && mDeformers.getHolds(slot) == 0)
                return false;

        return true;
    }

    bool SceneDesc::isEmpty() const
    {
        return mMeshes.getLiveCount() == 0 && mMaterials.getLiveCount() == 0 && mTextures.getLiveCount() == 0
            && mDeformers.getLiveCount() == 0 && mPlacements.getCounts().mPlaced == 0;
    }

    void SceneDesc::noteWalked()
    {
        mTurn.step(Turn::Walked, Turn::Open, Turn::Walked);
    }

    void SceneDesc::noteSwept()
    {
        // A sweep of a scene nothing walked since the last clear or the last hand-over — a world
        // detached, a second sweep — settles nothing and moves nothing.
        if (mTurn.get() == Turn::Walked)
            mTurn.step(Turn::Open, Turn::Walked);
    }

    void SceneDesc::orderLights()
    {
        // From open or handed, and never from walked: a scene handed over twice between clears is
        // handed the same lists twice, which is what a picture asked for again is, and a scene
        // built by hand was never walked. What may not come between is an addition, and what may
        // not come before is a walk whose sweep has not run.
        mTurn.step(Turn::Handed, Turn::Open, Turn::Handed);

        // A total order, so that two lights the walk could hand over either way round come out the
        // same way round every time. Tied and not built, because a tuple of references copies
        // nothing over thousands of comparisons.
        std::sort(mLights.begin(), mLights.end(), [](const Light& a, const Light& b) {
            return std::tie(a.mPosition, a.mIntensity, a.mReach, a.mSourceRadius, a.mClearance, a.mFill)
                < std::tie(b.mPosition, b.mIntensity, b.mReach, b.mSourceRadius, b.mClearance, b.mFill);
        });
    }

    void SceneDesc::clearPlacement()
    {
        // A walked scene stays walked: the lists go, the sweep is still owed.
        if (mTurn.get() != Turn::Walked)
            mTurn.step(Turn::Open, Turn::Open, Turn::Handed);

        mLights.clear();

        mMeshes.clearDeformed();
        mSprites.clear();
        mEmitters.clear();
        mRipples.clear();
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

        const std::size_t freedMeshes = mMeshes.sweep(mDeformers);
        const std::size_t freedMaterials = mMaterials.sweep(mTextures);

        // A sweep frees a row nothing holds and nothing named this walk; a placement still standing
        // on it would be traced against whatever the slot is next given to.
        assert(placementsStandOnLiveRows() && "a sweep freed a row a placement stands on");

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

    std::span<const PoseWord> SceneDesc::getMeshPose(const Index mesh) const
    {
        return mDeformers.getMeshPose(mMeshes.getRows()[mesh]);
    }

    template <class Visit>
    void SceneDesc::forEachPlacement(Visit&& visit) const
    {
        for (const PlacementRow& row : mPlacements.getRows())
        {
            const MeshInstance& instance = row.mInstance;
            if (!instance.isPlaced())
                continue;

            // Each mesh's own box carried through its instances, rather than every vertex of
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

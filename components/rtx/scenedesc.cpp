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
        assert(
            (material == sNoIndex || material < mMaterialTable.size()) && "a mesh wearing a material the scene lacks");

        return mMeshTable.add(arrays, shape, deform, deformer, material);
    }

    Index SceneDesc::addRig(
        std::span<const std::uint32_t> runs, std::span<const Shaders::GpuInfluence> influences, Index boneCount)
    {
        return mDeformers.addRig(runs, influences, boneCount);
    }

    Index SceneDesc::addMorph(std::span<const osg::Vec3f> offsets, Index targets)
    {
        return mDeformers.addMorph(offsets, targets);
    }

    void SceneDesc::poseRig(Index mesh, std::span<const Shaders::GpuBone> bones, const osg::BoundingBoxf& bounds)
    {
        assert(mesh < mMeshTable.size());
        if (mDeformers.poseRig(mMeshTable.getRows()[mesh], bones))
            mMeshTable.notePosed(mesh, bounds);
    }

    void SceneDesc::poseMorph(Index mesh, std::span<const float> weights, const osg::BoundingBoxf& bounds)
    {
        assert(mesh < mMeshTable.size());
        if (mDeformers.poseMorph(mMeshTable.getRows()[mesh], weights))
            mMeshTable.notePosed(mesh, bounds);
    }

    Index SceneDesc::addMaterial(const Material& material)
    {
        return mMaterialTable.add(material);
    }

    void SceneDesc::setMaterial(Index material, const Material& what)
    {
        if (!mMaterialTable.set(material, what))
            return;

        // Linear over the placements on the frame a surface crosses opaque, which a fade does twice
        // in its life; the flipbooks and the scrolls that animate every frame never come here.
        //
        // **Here and not in the table, because the placements are not the table's.** What a
        // material changed about traversal is the table's answer; which rows carry it is this.
        const std::span<const MeshInstance> placed = mPlacements.getAll();
        for (Index slot = 0; slot < placed.size(); ++slot)
            if (placed[slot].isPlaced() && placed[slot].mMaterial == material)
                mPlacements.rewrite(slot);
    }

    void SceneDesc::holdTexture(Index texture)
    {
        mTextures.hold(texture);
    }

    void SceneDesc::dropTexture(Index texture)
    {
        mTextures.drop(texture);
    }

    Run SceneDesc::addMask(std::span<const float> weights)
    {
        return mMaterialTable.addMask(weights);
    }

    Run SceneDesc::addLayers(std::span<const MaterialLayer> layers)
    {
        return mMaterialTable.addLayers(layers);
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

        // **A quad's reach is its own diagonal, not its size.** An eye-facing sprite is a disc of
        // `mRadius`; an oriented one is a rectangle as long as its axis and as wide as `width`, and
        // a rain streak ten times as tall as it is wide would be cut off by a sphere measured on the
        // width. The two are perpendicular wherever the march looks at the quad, because the width
        // is swung about the axis — so the corner is the diagonal of the two lengths.
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

    Index SceneDesc::addTexture(VFS::Path::NormalizedView path)
    {
        return mTextures.add(path);
    }

    Index SceneDesc::addBakedTexture(std::string_view key)
    {
        return mTextures.addBaked(key);
    }

    Index SceneDesc::addInstance(const MeshInstance& instance)
    {
        assert(instance.mMesh < mMeshTable.size());
        assert(instance.mMaterial == sNoIndex || instance.mMaterial < mMaterialTable.size());

        return mPlacements.add(instance);
    }

    void SceneDesc::fadeInstance(Index slot, float opacity)
    {
        mPlacements.fade(slot, opacity);
    }

    bool SceneDesc::moveInstance(Index slot, const osg::Matrixf& transform)
    {
        return mPlacements.move(slot, transform);
    }

    void SceneDesc::dropInstance(Index slot)
    {
        mPlacements.drop(slot);
    }

    void SceneDesc::orderLights()
    {
        // **A total order and not a distance**, so that two lights the walk could hand over either
        // way round come out the same way round every time. Position separates all but the lamps
        // standing in one another, and what they carry separates those.
        //
        // **Tied and not built**, because a sort of a cell's three hundred lamps compares thousands
        // of times and a tuple of references copies none of them. `osg::Vec3f` orders itself
        // lexicographically on x, y and z, which is what makes the two vectors here the same order
        // as their six components spelled out.
        std::sort(mLights.begin(), mLights.end(), [](const Light& a, const Light& b) {
            return std::tie(a.mPosition, a.mIntensity, a.mReach, a.mSourceRadius, a.mClearance)
                < std::tie(b.mPosition, b.mIntensity, b.mReach, b.mSourceRadius, b.mClearance);
        });
    }

    void SceneDesc::advancePlacement()
    {
        mPlacements.advance();
    }

    void SceneDesc::clearPlacement()
    {
        mLights.clear();

        mMeshTable.clearDeformed();
        mSprites.clear();
        mEmitters.clear();
    }

    bool SceneDesc::release(std::span<const Index> meshes, std::span<const Index> materials)
    {
        // Only meshes and materials are asked, and that is now the whole of what this frees: a
        // texture goes when the last material or hold naming it lets go, wherever that happens.
        const std::size_t keptMeshes = mMeshTable.mark(meshes);
        const std::size_t keptMaterials = mMaterialTable.mark(materials);

        // **The ordinary frame leaves here**: a table with as many survivors as live entries has
        // nothing to free, and what it paid for the answer is the marking above.
        //
        // **Asked of the marks and not of the span's length.** Those two agree only while the keep
        // set names each survivor once, which is a property of the identity map that fills it rather
        // than of this call — so a second way of collecting survivors cannot get it wrong.
        if (keptMeshes == mMeshTable.getLiveCount() && keptMaterials == mMaterialTable.getLiveCount())
            return false;

        const std::size_t freedMeshes = mMeshTable.sweep();
        const std::size_t freedMaterials = mMaterialTable.sweep();

        // **The per-frame lists are left as the walk left them.** Emptying them here read as "the
        // walk that comes next refills them", and that walk is the *next frame's* — one frame after
        // the picture this one is about to hand over, so a caller that uploads in between drew a
        // frame with no lights, sprites or emitters in it at all.
        //
        // Nothing in them can be stale either: a sweep is only sound straight after a walk of the
        // whole world (`SceneExtractor::retire`), so what is in them came from nodes that walk met —
        // the survivors, by the same marking this frees against.
        //
        // **Neither is a structure change, and neither is a shading change.** Nothing arrived and
        // nothing moved: the structures built from these indices are still correct, they simply
        // describe geometry nothing stands on any more, and the top level a frame rebuilds anyway is
        // what stops them being traced. A freed material's row, layers and masks are read by nothing
        // either, so no table has to be written for them — the next thing to land in the slot or the
        // run is what names it.
        return freedMeshes > 0 || freedMaterials > 0;
    }

    void SceneDesc::clearArrivals()
    {
        mMeshTable.clearArrivals();
        mTextures.clearArrivals();
        mMaterialTable.clearArrivals();
        mDeformers.clearArrivals();
    }

}

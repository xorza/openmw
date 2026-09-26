#include "cellplacer.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <osg/Matrixf>
#include <osg/Vec3f>

#include "cellgrid.hpp"
#include "held.hpp"
#include "lightbuilder.hpp"
#include "mesh.hpp"
#include "prepared.hpp"
#include "result.hpp"
#include "runs.hpp"
#include "scenedesc.hpp"
#include "shaders/scene.h"
#include "shapefold.hpp"
#include "surface.hpp"
#include "textureencoding.hpp"
#include "texturewrap.hpp"

namespace Rtx
{
    void CellPlacer::setReferenceEnabled(const ESM::RefNum refnum, const bool enabled, const std::span<HeldCell> held)
    {
        const auto at = std::lower_bound(mDisabled.begin(), mDisabled.end(), refnum);
        const bool known = at != mDisabled.end() && *at == refnum;

        if (enabled && known)
            mDisabled.erase(at);
        else if (!enabled && !known)
            mDisabled.insert(at, refnum);

        // A blacklisted reference stays down whatever the script says.
        if (enabled && std::binary_search(mBlacklisted.begin(), mBlacklisted.end(), refnum))
            return;

        setHeldEnabled(refnum, enabled, held);
    }

    void CellPlacer::blacklistReference(const ESM::RefNum refnum, const std::span<HeldCell> held)
    {
        const auto at = std::lower_bound(mBlacklisted.begin(), mBlacklisted.end(), refnum);
        if (at != mBlacklisted.end() && *at == refnum)
            return;

        mBlacklisted.insert(at, refnum);
        setHeldEnabled(refnum, false, held);
    }

    void CellPlacer::setHeldEnabled(const ESM::RefNum refnum, const bool enabled, const std::span<HeldCell> held)
    {
        // Every cell, because which one holds the reference is not said; a script's toggle is rare
        // enough that the walk is cheaper than an index kept for it.
        for (HeldCell& cell : held)
            for (std::size_t slot = 0; slot < cell.mPlacements.size(); ++slot)
                if (cell.mPlacements[slot].mRefNum == refnum)
                    setPlacementEnabled(cell.mPlacements[slot], slot < cell.mShown, enabled);
    }

    void CellPlacer::forgetReferences(const std::span<HeldCell> held)
    {
        mDisabled.clear();
        mBlacklisted.clear();

        for (HeldCell& cell : held)
            for (std::size_t slot = 0; slot < cell.mPlacements.size(); ++slot)
                if (cell.mPlacements[slot].mDisabled)
                    setPlacementEnabled(cell.mPlacements[slot], slot < cell.mShown, true);
    }

    void CellPlacer::setPlacementEnabled(Placement& placement, const bool shown, const bool enabled)
    {
        placement.mDisabled = !enabled;
        if (!shown)
            return;

        if (enabled && !placement.mStood.isStanding())
            stand(placement.mStood, mPlaced);
        else if (!enabled)
            drop(placement.mStood, mPlaced);
    }

    bool CellPlacer::isDisabled(const ESM::RefNum refnum) const
    {
        return (!mDisabled.empty() && std::binary_search(mDisabled.begin(), mDisabled.end(), refnum))
            || (!mBlacklisted.empty() && std::binary_search(mBlacklisted.begin(), mBlacklisted.end(), refnum));
    }

    bool CellPlacer::wantsFlattening(const osg::Vec2i& cell, const Material& ground, const WorldAround& around)
    {
        // A stack is flattened outside the active grid, where the quad tree flattens too, and a
        // single layer is never, because it is already a single fetch.
        return ground.mLayers.mCount > 1 && !inActiveGrid(cell, around.mActiveGrid);
    }

    void CellPlacer::adoptGround(
        const PreparedCell& cell, HeldCell& held, const WorldAround& around, ExtractionStats& stats)
    {
        const PreparedGround& ground = cell.mGround;
        if (!ground.mStands)
            return;

        HeldGround& stands = held.mGround.emplace();
        mLayerScratch.clear();
        bool mapped = false;
        for (const PreparedLayer& layer : ground.mLayers)
        {
            MaterialLayer row = layer.mRow;

            // A table with no room left names the neutral texel: the device reads a layer's slot
            // with no test, as it reads a material's diffuse.
            const Index slot = mScene.textures().add(layer.mTexture->mPath, layer.mTexture->mImage.get());
            row.mDiffuse = slot != sNoIndex ? slot : Shaders::TEXTURE_NEUTRAL;

            // A normal map is data and tiles with the diffuse; one the table has no room for is
            // no normal map, and the layer keeps the chunk's normal and its own coordinates.
            if (layer.mNormalTexture != nullptr)
            {
                row.mNormal = mScene.textures().add(layer.mNormalTexture->mPath, layer.mNormalTexture->mImage.get(),
                    TextureWrap::Repeat, TextureEncoding::Data);
                stands.mTextures.push_back(layer.mNormalTexture);
                if (layer.mParallax && row.mNormal != sNoIndex)
                    row.mFlags |= Shaders::LAYER_PARALLAX;
            }

            // What a `_diffusespec`'s alpha is, the layout says: a classic one is a highlight's
            // strength, which this renderer has no use for, and its colour is a diffuse like any.
            if (layer.mDiffuseSpec && mSpecularLayout == SpecularLayout::MetalRoughness)
                row.mFlags |= Shaders::LAYER_AUTHORED;

            mapped = mapped || row.mNormal != sNoIndex || row.mFlags != 0;

            // The row keeps no count: `maskOf` reads the grid's area back, so the run
            // the table hands out has to be exactly that long, which the reader asserts as it reads.
            if (!layer.mWeights.empty())
            {
                const Run mask = mScene.materials().addMask(layer.mWeights.in(std::span<const float>(ground.mWeights)));
                assert(mask.mCount == row.mMaskWidth * row.mMaskHeight && "a mask run that is not its grid's area");
                row.mMaskOffset = mask.mOffset;
            }

            mLayerScratch.push_back(row);

            stands.mTextures.push_back(layer.mTexture);
        }

        stands.mStood.mTransform = osg::Matrixf::translate(ground.mOrigin);

        Material material;
        material.mKind = MaterialKind::Terrain;

        // Stated here, because the ground is nobody's node. Every other material reads its
        // mode off a state set `NifOsg` described, and this one is stood off the land records —
        // where `Terrain::ChunkManager` states the same thing for the rasterizer's chunks.
        material.mVertexColour = VertexColour::Tint;
        material.mLayersMapped = mapped;
        if (!mLayerScratch.empty())
            material.mLayers = mScene.materials().addLayers(mLayerScratch);
        material.mFlatten = wantsFlattening(held.mCell, material, around);
        stands.mStood.mMaterial = mScene.addMaterial(material);

        // A heightfield is neither a sheet nor closed, and no fold is needed to say so.
        stands.mStood.mMesh = mScene.addMesh(MeshArrays{ .mPositions = ground.mPositions,
                                                 .mNormals = ground.mNormals,
                                                 .mTexCoords = ground.mTexCoords,
                                                 .mColours = ground.mColours,
                                                 .mIndices = ground.mIndices },
            FoldedShape{});

        // Held on the scene, because no drawable and no state set will ever name them. The
        // sweep keeps a held row, and `dropGround` is what lets go.
        mScene.meshes().hold(stands.mStood.mMesh);
        mScene.materials().hold(stands.mStood.mMaterial);

        ++stats.mMeshesAdded;
        ++stats.mMaterialsAdded;
    }

    void CellPlacer::adoptPlacements(const PreparedCell& cell, HeldCell& held, CellHolds& holds)
    {
        held.mPlacements.clear();
        held.mShown = 0;

        for (const PreparedRef& ref : cell.mRefs)
        {
            const PreparedModel& model = *cell.mModels[ref.mModel];
            const CellHolds::HeldModel& adopted = holds.knownOf(model);
            const bool disabled = isDisabled(ref.mRefNum);

            for (std::size_t at = 0; at < adopted.mParts.size(); ++at)
                held.mPlacements.push_back(Placement{
                    .mStood = {
                        .mMesh = adopted.mParts[at].mMesh,
                        .mMaterial = adopted.mParts[at].mMaterial,
                        .mTransform = model.mParts[at].mLocal * ref.mTransform,
                    },
                    .mRadius = ref.mRadius,
                    .mRefNum = ref.mRefNum,
                    .mDisabled = disabled,
                });
        }

        // Largest first, once, so the size rule's answer is a prefix on every walk after this.
        // Stable, so equal radii keep the order the references were read in and two runs of one
        // walk place the same slots.
        std::stable_sort(held.mPlacements.begin(), held.mPlacements.end(),
            [](const Placement& larger, const Placement& smaller) { return larger.mRadius > smaller.mRadius; });

        held.mLights.assign(cell.mLights.begin(), cell.mLights.end());
    }

    void CellPlacer::stand(Stood& stood, std::uint32_t& standing)
    {
        assert(!stood.isStanding() && "a thing stood twice");

        stood.mSlot = mScene.addInstance(MeshInstance{
            .mTransform = stood.mTransform,
            .mMesh = stood.mMesh,
            .mMaterial = stood.mMaterial,
            .mStander = Stander::Ring,
        });
        ++standing;
    }

    void CellPlacer::drop(Stood& stood, std::uint32_t& standing)
    {
        if (!stood.isStanding())
            return;

        mScene.placements().drop(stood.mSlot, Stander::Ring);
        stood.mSlot = sNoIndex;
        --standing;
    }

    void CellPlacer::dropGround(HeldCell& cell)
    {
        if (!cell.mGround.has_value())
            return;

        HeldGround& ground = *cell.mGround;
        drop(ground.mStood, mGroundPlaced);

        // The rows lose their holds, and the sweep after this walk is what frees them.
        mScene.meshes().drop(ground.mStood.mMesh);
        mScene.materials().drop(ground.mStood.mMaterial);
        ground.reuse();
    }

    void CellPlacer::dropSlots(HeldCell& cell)
    {
        for (std::size_t at = 0; at < cell.mShown; ++at)
            drop(cell.mPlacements[at].mStood, mPlaced);
        cell.mShown = 0;

        if (cell.mGround.has_value())
            drop(cell.mGround->mStood, mGroundPlaced);
    }

    bool CellPlacer::standsAsHeld(const HeldCell& cell, const WorldAround& around) const
    {
        const std::span<const PlacementRow> placed = mScene.placements().getRows();

        // Whether `stood` stands exactly where it says, or stands nowhere where `wanted` is false.
        const auto standsAs = [&](const Stood& stood, const bool wanted) {
            if (wanted != stood.isStanding())
                return false;
            if (!wanted)
                return true;
            if (stood.mSlot >= placed.size())
                return false;

            const MeshInstance& standing = placed[stood.mSlot].mInstance;
            return standing.isPlaced() && standing.mStander == Stander::Ring && standing.mMesh == stood.mMesh
                && standing.mMaterial == stood.mMaterial;
        };

        const bool inReach = around.mExterior && withinReach(cell.mCell, around.mEye, around.mReach);
        const bool shown = inReach && !inActiveGrid(cell.mCell, around.mActiveGrid);
        if (!shown && cell.mShown != 0)
            return false;

        for (std::size_t at = 0; at < cell.mPlacements.size(); ++at)
        {
            const Placement& placement = cell.mPlacements[at];
            if (!standsAs(placement.mStood, at < cell.mShown && !placement.mDisabled))
                return false;
        }

        if (cell.mGround.has_value() && !standsAs(cell.mGround->mStood, inReach))
            return false;

        return true;
    }

    bool CellPlacer::standsNoMore() const
    {
        std::uint32_t standing = 0;
        for (const PlacementRow& row : mScene.placements().getRows())
            if (row.mInstance.isPlaced() && row.mInstance.mStander == Stander::Ring)
                ++standing;

        return standing == getPlaced() + getGroundPlaced();
    }

    std::uint32_t CellPlacer::place(HeldCell& cell, const WorldAround& around)
    {
        const bool inReach = withinReach(cell.mCell, around.mEye, around.mReach);

        // The ground stands inside the active grid too: the game builds none for this
        // renderer, so what a cell's land says is stood here wherever the cell is.
        if (cell.mGround.has_value())
        {
            HeldGround& ground = *cell.mGround;

            if (inReach && !ground.mStood.isStanding())
                stand(ground.mStood, mGroundPlaced);
            else if (!inReach)
                drop(ground.mStood, mGroundPlaced);

            // A cell crossing the grid's edge shades the other way from now on. The composite it
            // held goes with the rewrite, and one it now wants is asked for by the row.
            const Material& stood = mScene.materials().getRows()[ground.mStood.mMaterial];
            if (const bool wanted = wantsFlattening(cell.mCell, stood, around); wanted != stood.mFlatten)
            {
                Material given = stood;
                given.mFlatten = wanted;
                given.mDiffuse = sNoIndex;
                given.mSpecular = sNoIndex;
                mScene.setMaterial(ground.mStood.mMaterial, given);
            }
        }

        const bool shown = inReach && !inActiveGrid(cell.mCell, around.mActiveGrid);

        // The paging's own rule: a reference is placed while its scaled radius clears the size
        // threshold at the eye's distance to its cell. The placements are sorted largest first, so
        // what clears is a prefix and where it ends is one search — and what this walk touches is
        // what entered or left that prefix since the last one, which on a standing frame is nothing.
        const float threshold = shown ? mMinSize * chebyshevDistanceTo(cell.mCell, around.mEye) : 0.0f;
        const float threshold2 = threshold * threshold;

        const std::span<Placement> placements = cell.mPlacements;
        const auto clears
            = [threshold2](const Placement& placement) { return placement.mRadius * placement.mRadius >= threshold2; };
        const std::size_t wanted = shown
            ? static_cast<std::size_t>(
                std::partition_point(placements.begin(), placements.end(), clears) - placements.begin())
            : 0;

        for (std::size_t at = cell.mShown; at < wanted; ++at)
            if (!placements[at].mDisabled)
                stand(placements[at].mStood, mPlaced);
        for (std::size_t at = wanted; at < cell.mShown; ++at)
            drop(placements[at].mStood, mPlaced);
        cell.mShown = wanted;

        // On every walk rather than kept, because the walk empties the lights and a flame is a
        // function of the hour; what is kept is the record.
        if (!shown)
            return 0;

        std::uint32_t lit = 0;
        for (const PreparedLight& lamp : cell.mLights)
        {
            const Result<std::optional<Light>, std::string_view> light = makeLight(
                lamp.mRecord, lamp.mPosition, around.mSimulationTime, static_cast<int>(lamp.mRefNum.mIndex));
            assert(light.isOk() && "a lamp the reader carried that the frame refuses");
            if (!light.value().has_value())
                continue;

            mScene.addLight(*light.value());
            ++lit;
        }

        return lit;
    }
}

#include "cellplacer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <osg/Matrixf>
#include <osg/Vec3f>

#include "cellgrid.hpp"
#include "held.hpp"
#include "lightbuilder.hpp"
#include "mesh.hpp"
#include "prepared.hpp"
#include "runs.hpp"
#include "scenedesc.hpp"
#include "shapefold.hpp"
#include "surface.hpp"

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

        // Every cell, because which one holds the reference is not said; a script's toggle is rare
        // enough that the walk is cheaper than an index kept for it.
        for (HeldCell& cell : held)
            for (std::size_t slot = 0; slot < cell.mPlacements.size(); ++slot)
            {
                Placement& placement = cell.mPlacements[slot];
                if (placement.mRefNum != refnum)
                    continue;

                placement.mDisabled = !enabled;
                if (slot >= cell.mShown)
                    continue;

                if (enabled && placement.mSlot == sNoIndex)
                    addSlot(placement);
                else if (!enabled)
                    dropSlot(placement);
            }
    }

    bool CellPlacer::isDisabled(const ESM::RefNum refnum) const
    {
        return !mDisabled.empty() && std::binary_search(mDisabled.begin(), mDisabled.end(), refnum);
    }

    bool CellPlacer::wantsFlattening(const osg::Vec2i& cell, const HeldGround& ground, const WorldAround& around)
    {
        // A stack is flattened outside the active grid, where the quad tree flattens too, and a
        // single layer is never, because it is already a single fetch.
        return ground.mLayers > 1 && !inActiveGrid(cell, around.mActiveGrid);
    }

    void CellPlacer::adoptGround(
        const PreparedCell& cell, HeldCell& held, CellHolds& holds, const WorldAround& around, ExtractionStats& stats)
    {
        const PreparedGround& ground = cell.mGround;
        if (!ground.mStands)
            return;

        HeldGround& stands = held.mGround.emplace();
        mLayerScratch.clear();
        for (const PreparedLayer& layer : ground.mLayers)
        {
            MaterialLayer row;
            row.mDiffuse = mScene.textures().add(layer.mTexture->mPath);
            row.mDiffuseTransform = layer.mDiffuseTransform;

            if (!layer.mWeights.empty())
            {
                row.mMask = mScene.materials().addMask(layer.mWeights.in(std::span<const float>(ground.mWeights)));
                row.mMaskWidth = layer.mMaskWidth;
                row.mMaskHeight = layer.mMaskHeight;
                row.mMaskTransform = layer.mMaskTransform;
            }

            mLayerScratch.push_back(row);

            stands.mTextures.push_back(layer.mTexture);
            holds.holdTexture(*layer.mTexture);
        }

        stands.mLayers = static_cast<std::uint32_t>(mLayerScratch.size());
        stands.mOrigin = ground.mOrigin;
        stands.mFlattened = wantsFlattening(held.mCell, stands, around);

        Material material;
        material.mKind = MaterialKind::Terrain;
        material.mFlatten = stands.mFlattened;

        // Stated here, because the ground is nobody's node. Every other material reads its
        // mode off a state set `NifOsg` described, and this one is stood off the land records —
        // where `Terrain::ChunkManager` states the same thing for the rasterizer's chunks.
        material.mVertexColour = VertexColour::Tint;
        if (!mLayerScratch.empty())
            material.mLayers = mScene.materials().addLayers(mLayerScratch);
        stands.mMaterial = mScene.materials().add(material);

        // A heightfield is neither a sheet nor closed, and no fold is needed to say so.
        stands.mMesh = mScene.addMesh(MeshArrays{ .mPositions = ground.mPositions,
                                          .mNormals = ground.mNormals,
                                          .mTexCoords = ground.mTexCoords,
                                          .mColours = ground.mColours,
                                          .mIndices = ground.mIndices },
            FoldedShape{}, Deform::None, sNoIndex);

        // Held on the scene, because no drawable and no state set will ever name them. The
        // sweep keeps a held row, and `dropGround` is what lets go.
        mScene.meshes().hold(stands.mMesh);
        mScene.materials().hold(stands.mMaterial);

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
                    .mMesh = adopted.mParts[at].mMesh,
                    .mMaterial = adopted.mParts[at].mMaterial,
                    .mTransform = model.mParts[at].mLocal * ref.mTransform,
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

    void CellPlacer::addSlot(Placement& placement)
    {
        placement.mSlot = mScene.addInstance(MeshInstance{
            .mTransform = placement.mTransform,
            .mMesh = placement.mMesh,
            .mMaterial = placement.mMaterial,
            .mStander = Stander::Ring,
        });
        ++mPlaced;
    }

    void CellPlacer::dropSlot(Placement& placement)
    {
        if (placement.mSlot == sNoIndex)
            return;

        mScene.placements().drop(placement.mSlot, Stander::Ring);
        placement.mSlot = sNoIndex;
        --mPlaced;
    }

    void CellPlacer::dropSlot(HeldGround& ground)
    {
        if (ground.mSlot == sNoIndex)
            return;

        mScene.placements().drop(ground.mSlot, Stander::Ring);
        ground.mSlot = sNoIndex;
        --mGroundPlaced;
    }

    void CellPlacer::dropGround(HeldCell& cell, CellHolds& holds)
    {
        if (!cell.mGround.has_value())
            return;

        HeldGround& ground = *cell.mGround;
        dropSlot(ground);

        for (PreparedTexture* texture : ground.mTextures)
            holds.dropTexture(*texture);

        // The rows lose their holds, and the sweep after this walk is what frees them.
        mScene.meshes().drop(ground.mMesh);
        mScene.materials().drop(ground.mMaterial);
        ground.reuse();
    }

    void CellPlacer::dropSlots(HeldCell& cell)
    {
        for (std::size_t at = 0; at < cell.mShown; ++at)
            dropSlot(cell.mPlacements[at]);
        cell.mShown = 0;

        if (cell.mGround.has_value())
            dropSlot(*cell.mGround);
    }

    bool CellPlacer::standsAsHeld(const HeldCell& cell, const WorldAround& around) const
    {
        const std::span<const MeshInstance> placed = mScene.placements().getAll();
        const auto stands = [&](const Index slot, const Index mesh, const Index material) {
            return slot < placed.size() && placed[slot].isPlaced() && placed[slot].mStander == Stander::Ring
                && placed[slot].mMesh == mesh && placed[slot].mMaterial == material;
        };

        const bool inReach = around.mExterior && withinReach(cell.mCell, around.mEye, around.mReach);
        const bool shown = inReach && !inActiveGrid(cell.mCell, around.mActiveGrid);
        if (!shown && cell.mShown != 0)
            return false;

        for (std::size_t at = 0; at < cell.mPlacements.size(); ++at)
        {
            const Placement& placement = cell.mPlacements[at];
            const bool wanted = at < cell.mShown && !placement.mDisabled;
            if (wanted != (placement.mSlot != sNoIndex))
                return false;
            if (wanted && !stands(placement.mSlot, placement.mMesh, placement.mMaterial))
                return false;
        }

        if (cell.mGround.has_value())
        {
            const HeldGround& ground = *cell.mGround;
            if (inReach != (ground.mSlot != sNoIndex))
                return false;
            if (inReach && !stands(ground.mSlot, ground.mMesh, ground.mMaterial))
                return false;
        }

        return true;
    }

    bool CellPlacer::standsNoMore() const
    {
        std::uint32_t standing = 0;
        for (const MeshInstance& placed : mScene.placements().getAll())
            if (placed.isPlaced() && placed.mStander == Stander::Ring)
                ++standing;

        return standing == mPlaced + mGroundPlaced;
    }

    std::uint32_t CellPlacer::place(HeldCell& cell, const WorldAround& around)
    {
        const bool inReach = withinReach(cell.mCell, around.mEye, around.mReach);

        // The ground stands inside the active grid too: the game builds none for this
        // renderer, so what a cell's land says is stood here wherever the cell is.
        if (cell.mGround.has_value())
        {
            HeldGround& ground = *cell.mGround;

            if (inReach && ground.mSlot == sNoIndex)
            {
                ground.mSlot = mScene.addInstance(MeshInstance{
                    .mTransform = osg::Matrixf::translate(ground.mOrigin),
                    .mMesh = ground.mMesh,
                    .mMaterial = ground.mMaterial,
                    .mStander = Stander::Ring,
                });
                ++mGroundPlaced;
            }
            else if (!inReach)
                dropSlot(ground);

            // A cell crossing the grid's edge shades the other way from now on. The composite it
            // held goes with the rewrite, and one it now wants is asked for by the row.
            if (wantsFlattening(cell.mCell, ground, around) != ground.mFlattened)
            {
                Material given = mScene.materials().getRows()[ground.mMaterial];
                given.mFlatten = !ground.mFlattened;
                given.mDiffuse = sNoIndex;
                mScene.setMaterial(ground.mMaterial, given);
                ground.mFlattened = given.mFlatten;
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
                addSlot(placements[at]);
        for (std::size_t at = wanted; at < cell.mShown; ++at)
            dropSlot(placements[at]);
        cell.mShown = wanted;

        // On every walk rather than kept, because the walk empties the lights and a flame is a
        // function of the hour; what is kept is the record.
        if (!shown)
            return 0;

        std::uint32_t lit = 0;
        for (const PreparedLight& lamp : cell.mLights)
        {
            const std::optional<Light> light = makeLight(
                lamp.mRecord, lamp.mPosition, around.mSimulationTime, static_cast<int>(lamp.mRefNum.mIndex));
            if (!light.has_value())
                continue;

            mScene.addLight(*light);
            ++lit;
        }

        return lit;
    }
}

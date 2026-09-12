#include "cellplacer.hpp"

#include <algorithm>
#include <cstddef>
#include <span>

#include <components/surface/vertexcolour.hpp>

#include "cellholds.hpp"
#include "distantland.hpp"
#include "mesharrays.hpp"
#include "meshinstance.hpp"
#include "meshrange.hpp"
#include "preparedcell.hpp"
#include "scenedesc.hpp"

namespace Rtx
{
    namespace
    {
        /// How far the eye stands from the nearest point of `cell`, in units, by the square's own
        /// metric — which is the one the paging measured a chunk's reach by.
        float distanceTo(const osg::Vec2i& cell, const osg::Vec3f& eye)
        {
            const float low = static_cast<float>(cell.x()) * sCellSize;
            const float high = low + sCellSize;
            const float lowY = static_cast<float>(cell.y()) * sCellSize;
            const float highY = lowY + sCellSize;

            const float alongX = std::max({ low - eye.x(), eye.x() - high, 0.0f });
            const float alongY = std::max({ lowY - eye.y(), eye.y() - highY, 0.0f });

            return std::max(alongX, alongY);
        }
    }

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

    bool CellPlacer::inActiveGrid(const osg::Vec2i& cell, const WorldAround& around)
    {
        return cell.x() >= around.mActiveGrid.x() && cell.y() >= around.mActiveGrid.y()
            && cell.x() < around.mActiveGrid.z() && cell.y() < around.mActiveGrid.w();
    }

    bool CellPlacer::isDisabled(const ESM::RefNum refnum) const
    {
        return !mDisabled.empty() && std::binary_search(mDisabled.begin(), mDisabled.end(), refnum);
    }

    bool CellPlacer::wantsFlattening(const osg::Vec2i& cell, const HeldGround& ground, const WorldAround& around)
    {
        // **A stack is flattened outside the active grid, and a single layer is never.** Inside
        // the grid the ground is near enough that the sharpness of a live stack is worth its cost
        // per hit, which is the rule the quad tree reached at about a cell out; a single layer is
        // already a single fetch, and flattening one would only resample a tiling texture into
        // something coarser than its file.
        return ground.mLayers > 1 && !inActiveGrid(cell, around);
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

        // **The material before the mesh**, here too: a mesh records the material it arrives
        // wearing, and a cell's ground wears one for its life.
        Material material;
        material.mKind = MaterialKind::Terrain;
        material.mFlatten = stands.mFlattened;

        // **Stated here, because the ground is nobody's node.** Every other material reads its
        // mode off a state set `NifOsg` described, and this one is stood off the land records —
        // where `Terrain::ChunkManager` states the same thing for the rasterizer's chunks.
        material.mVertexColour = Surface::VertexColour::Tint;
        if (!mLayerScratch.empty())
            material.mLayers = mScene.materials().addLayers(mLayerScratch);
        stands.mMaterial = mScene.materials().add(material);

        // A heightfield is neither a sheet nor closed, and no fold is needed to say so.
        stands.mMesh = mScene.addMesh(MeshArrays{ .mPositions = ground.mPositions,
                                          .mNormals = ground.mNormals,
                                          .mTexCoords = ground.mTexCoords,
                                          .mColours = ground.mColours,
                                          .mIndices = ground.mIndices },
            FoldedShape{}, Deform::None, sNoIndex, stands.mMaterial);

        // **Held on the scene, because no drawable and no state set will ever name them.** The
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
    }

    void CellPlacer::addSlot(Placement& placement)
    {
        placement.mSlot = mScene.addInstance(MeshInstance{
            .mTransform = placement.mTransform,
            .mMesh = placement.mMesh,
            .mMaterial = placement.mMaterial,
        });
        ++mPlaced;
    }

    void CellPlacer::dropSlot(Placement& placement)
    {
        if (placement.mSlot == sNoIndex)
            return;

        mScene.placements().drop(placement.mSlot);
        placement.mSlot = sNoIndex;
        --mPlaced;
    }

    void CellPlacer::dropSlot(HeldGround& ground)
    {
        if (ground.mSlot == sNoIndex)
            return;

        mScene.placements().drop(ground.mSlot);
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

    void CellPlacer::place(HeldCell& cell, const WorldAround& around, const osg::Vec2i& eye, const int reach)
    {
        const bool inReach = withinCells(cell.mCell, eye, reach);

        // **The ground stands inside the active grid too**: the game builds none for this
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
                });
                ++mGroundPlaced;
            }
            else if (!inReach)
                dropSlot(ground);

            // A cell crossing the grid's edge shades the other way from now on. The composite it
            // held goes with the rewrite, and one it now wants is asked for by the row.
            if (wantsFlattening(cell.mCell, ground, around) != ground.mFlattened)
            {
                Material given = mScene.getTables().mMaterials.getRows()[ground.mMaterial];
                given.mFlatten = !ground.mFlattened;
                given.mDiffuse = sNoIndex;
                mScene.setMaterial(ground.mMaterial, given);
                ground.mFlattened = given.mFlatten;
            }
        }

        const bool shown = inReach && !inActiveGrid(cell.mCell, around);

        // The paging's own rule: a reference is placed while its scaled radius clears the size
        // threshold at the eye's distance to its cell. The placements are sorted largest first, so
        // what clears is a prefix and where it ends is one search — and what this walk touches is
        // what entered or left that prefix since the last one, which on a standing frame is nothing.
        const float threshold = shown ? mMinSize * distanceTo(cell.mCell, around.mEye) : 0.0f;
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
    }
}

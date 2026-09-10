#include "cellring.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <span>
#include <utility>

#include <components/misc/constants.hpp>

#include "distantland.hpp"
#include "materialresolver.hpp"
#include "meshinstance.hpp"
#include "meshrange.hpp"
#include "meshreader.hpp"
#include "scenedesc.hpp"

namespace Rtx
{
    namespace
    {
        constexpr float sCellSize = static_cast<float>(Constants::CellSizeInUnits);

        /// How many cells past the reach are prepared before they can be seen.
        ///
        /// **One, which at the island route's speed is most of a second and at a walk is four.** A
        /// cell's ground and models are adopted and its structures built while it is still a cell
        /// away from being placed, so the frame it crosses into the reach owes only its placements.
        constexpr int sPreparedBand = 1;

        /// The order the prepared ring's missing cells are read in: nearest first, then a fixed
        /// order among equals, so two runs from one eye ask for one list.
        struct Nearer
        {
            osg::Vec2i mEye;

            bool operator()(const osg::Vec2i& left, const osg::Vec2i& right) const
            {
                const int leftAway = std::max(std::abs(left.x() - mEye.x()), std::abs(left.y() - mEye.y()));
                const int rightAway = std::max(std::abs(right.x() - mEye.x()), std::abs(right.y() - mEye.y()));
                if (leftAway != rightAway)
                    return leftAway < rightAway;

                return left < right;
            }
        };

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

        bool withinBand(const osg::Vec2i& cell, const osg::Vec2i& eye, const int band)
        {
            return std::abs(cell.x() - eye.x()) <= band && std::abs(cell.y() - eye.y()) <= band;
        }

        /// The order the held cells are kept in: `osg::Vec2i` orders lexicographically already,
        /// and a cell and a held cell are both asked by it.
        const osg::Vec2i& cellKey(const osg::Vec2i& cell)
        {
            return cell;
        }

        template <class Held>
        const osg::Vec2i& cellKey(const Held& held)
        {
            return held.mCell;
        }

    }

    CellRing::CellRing(SceneDesc& scene)
        : mScene(scene)
    {
    }

    CellRing::~CellRing() = default;

    void CellRing::setContent(
        Terrain::Storage* const ground, ContentSource* const content, const osg::Node::NodeMask mask)
    {
        mGround = ground;
        mContent = content;
        mMask = mask;
    }

    void CellRing::follow(const WorldAround& around)
    {
        mAround = around;

        const CellWorld world{
            .mStorage = around.mStorage,
            .mGround = mGround,
            .mContent = mContent,
            .mWorldspace = around.mWorldspace,
            .mMask = mMask,
        };

        if (mSupply.isReading(world))
            return;

        // **Everything held names the reader that is about to go**, so it is let go of before the
        // supply is pointed anywhere else. Nothing is given back: what the frame held dies with the
        // reader that lent it.
        forget();
        mSupply.follow(world);
    }

    void CellRing::forget()
    {
        std::uint32_t grounds = 0;
        for (HeldCell& cell : mCells)
        {
            dropSlots(cell);
            grounds += cell.mGround.has_value() ? 1 : 0;
        }
        mCount.mMeshesDisowned += grounds;
        mCount.mMaterialsDisowned += grounds;

        mCells.clear();
        mModels.clear();
        mTextures.clear();
        mPending.clear();
    }

    void CellRing::setStaticsEnabled(const bool enabled)
    {
        mStatics = enabled;
    }

    void CellRing::setMinSize(const float minSize)
    {
        mMinSize = minSize;
    }

    void CellRing::setFrame(const std::size_t frame)
    {
        mFrame = frame;
    }

    void CellRing::setSettled(const bool settled)
    {
        mSettled = settled;
    }

    void CellRing::setReferenceEnabled(const ESM::RefNum refnum, const bool enabled)
    {
        const auto at = std::lower_bound(mDisabled.begin(), mDisabled.end(), refnum);
        const bool held = at != mDisabled.end() && *at == refnum;

        if (enabled && held)
            mDisabled.erase(at);
        else if (!enabled && !held)
            mDisabled.insert(at, refnum);
    }

    const PreparedTexture* CellRing::find(const osg::Image& image) const
    {
        const auto at = std::lower_bound(mTextures.begin(), mTextures.end(), &image,
            [](const HeldTexture& held, const osg::Image* wanted) { return held.mImage < wanted; });
        if (at == mTextures.end() || at->mImage != &image)
            return nullptr;

        return at->mTexture;
    }

    bool CellRing::inActiveGrid(const osg::Vec2i& cell) const
    {
        return cell.x() >= mAround.mActiveGrid.x() && cell.y() >= mAround.mActiveGrid.y()
            && cell.x() < mAround.mActiveGrid.z() && cell.y() < mAround.mActiveGrid.w();
    }

    bool CellRing::holds(const osg::Vec2i& cell) const
    {
        return std::binary_search(mCells.begin(), mCells.end(), cell,
            [](const auto& left, const auto& right) { return cellKey(left) < cellKey(right); });
    }

    bool CellRing::pending(const osg::Vec2i& cell) const
    {
        return std::any_of(
            mPending.begin(), mPending.end(), [&](const PreparedCell* held) { return held->mCell == cell; });
    }

    bool CellRing::isDisabled(const ESM::RefNum refnum) const
    {
        return !mDisabled.empty() && std::binary_search(mDisabled.begin(), mDisabled.end(), refnum);
    }

    int CellRing::reachInCells() const
    {
        return static_cast<int>(std::ceil(mAround.mReach / sCellSize));
    }

    bool CellRing::wantsFlattening(const osg::Vec2i& cell, const HeldGround& ground) const
    {
        // **A stack is flattened outside the active grid, and a single layer is never.** Inside
        // the grid the ground is near enough that the sharpness of a live stack is worth its cost
        // per hit, which is the rule the quad tree reached at about a cell out; a single layer is
        // already a single fetch, and flattening one would only resample a tiling texture into
        // something coarser than its file.
        return ground.mLayers > 1 && !inActiveGrid(cell);
    }

    void CellRing::holdTexture(const PreparedTexture& texture)
    {
        const osg::Image* const image = texture.mImage.get();
        const auto at = std::lower_bound(mTextures.begin(), mTextures.end(), image,
            [](const HeldTexture& held, const osg::Image* wanted) { return held.mImage < wanted; });
        if (at != mTextures.end() && at->mImage == image)
            ++at->mHolders;
        else
            mTextures.insert(at, HeldTexture{ .mImage = image, .mTexture = &texture, .mHolders = 1 });
    }

    void CellRing::dropTexture(const PreparedTexture& texture)
    {
        const osg::Image* const image = texture.mImage.get();
        const auto at = std::lower_bound(mTextures.begin(), mTextures.end(), image,
            [](const HeldTexture& held, const osg::Image* wanted) { return held.mImage < wanted; });
        assert(at != mTextures.end() && at->mImage == image && "a reading dropped that was never held");

        if (--at->mHolders == 0)
            mTextures.erase(at);
    }

    CellRing::HeldModel& CellRing::know(PreparedModel& model)
    {
        const auto at = std::lower_bound(mModels.begin(), mModels.end(), &model,
            [](const HeldModel& held, const PreparedModel* wanted) { return held.mModel < wanted; });
        if (at != mModels.end() && at->mModel == &model)
            return *at;

        HeldModel known;
        if (!mSpareModels.empty())
        {
            known = std::move(mSpareModels.back());
            mSpareModels.pop_back();
        }

        known.mModel = &model;
        known.mParts.clear();
        known.mHeld = 0;
        known.mPending = 0;

        // The images the model names, for `find`: counted per model that names them.
        for (const PreparedTexture* texture : model.mTextures)
            holdTexture(*texture);

        return *mModels.insert(at, std::move(known));
    }

    CellRing::HeldModel& CellRing::knownOf(const PreparedModel& model)
    {
        const auto at = std::lower_bound(mModels.begin(), mModels.end(), &model,
            [](const HeldModel& held, const PreparedModel* wanted) { return held.mModel < wanted; });
        assert(at != mModels.end() && at->mModel == &model && "a model the frame was never told of");

        return *at;
    }

    void CellRing::release(PreparedModel& model, const bool wasHeld)
    {
        const auto at = std::lower_bound(mModels.begin(), mModels.end(), &model,
            [](const HeldModel& held, const PreparedModel* wanted) { return held.mModel < wanted; });
        assert(at != mModels.end() && at->mModel == &model && "a model released that the frame never knew of");

        HeldModel& known = *at;
        if (wasHeld)
            --known.mHeld;
        else
            --known.mPending;

        if (known.mHeld > 0 || known.mPending > 0)
            return;

        for (const PreparedTexture* texture : model.mTextures)
            dropTexture(*texture);

        mSpareModels.push_back(std::move(known));
        mModels.erase(at);
    }

    void CellRing::giveBackHolds(
        const std::span<PreparedModel* const> models, const std::span<PreparedTexture* const> textures)
    {
        CellReturns& back = mSupply.giveBack();
        back.mModels.insert(back.mModels.end(), models.begin(), models.end());
        back.mTextures.insert(back.mTextures.end(), textures.begin(), textures.end());
    }

    void CellRing::takeDone()
    {
        mDoneScratch.clear();
        mSupply.take(mDoneScratch);

        for (PreparedCell* cell : mDoneScratch)
        {
            // Counted as it arrives, so the frame knows of every model a cell it may adopt names.
            for (PreparedModel* model : cell->mModels)
                ++know(*model).mPending;

            // **The switch is a setting the game can move while it runs**, and a cell the thread
            // read under the other answer is read again rather than stood as it was.
            if (cell->mStatics != mStatics)
                discard(*cell);
            else
                mPending.push_back(cell);
        }

        mDoneScratch.clear();
    }

    void CellRing::ask(const osg::Vec2i& eye, const int band)
    {
        mAsking.mCells.clear();
        mAsking.mStatics = mStatics;

        for (int x = eye.x() - band; x <= eye.x() + band; ++x)
            for (int y = eye.y() - band; y <= eye.y() + band; ++y)
            {
                const osg::Vec2i cell(x, y);
                if (!holds(cell) && !pending(cell))
                    mAsking.mCells.push_back(cell);
            }

        std::sort(mAsking.mCells.begin(), mAsking.mCells.end(), Nearer{ eye });

        mSupply.ask(mAsking);
    }

    void CellRing::waitForNext()
    {
        // A cell read under the other answer to the statics switch is discarded rather than made
        // pending, so a wait that found one has not found what it waited for.
        while (mPending.empty())
        {
            mSupply.waitForOne();
            takeDone();
        }
    }

    void CellRing::adoptPending()
    {
        // **One cell a frame, and one frame walked twice adopts once.** A cell's meshes are copied
        // into the scene and its structures built by the hand-over that follows; two on one frame
        // would be the batch behind a threshold this renderer never takes. A settled walk keeps the
        // rule and waits for its one cell, which is what `setSettled` says.
        if (mPending.empty() || mAdoptedFrame == mFrame)
            return;

        mAdoptedFrame = mFrame;
        adopt(*mPending.front());
        mPending.erase(mPending.begin());
    }

    void CellRing::adoptParts(HeldModel& held)
    {
        const PreparedModel& model = *held.mModel;
        held.mParts.reserve(model.mParts.size());

        for (const PreparedPart& part : model.mParts)
        {
            // **The material before the mesh**, as the walk resolves them: a mesh records the
            // material it arrives wearing.
            const MaterialResolver::Resolved material = walk().adoptMaterial(part.mMaterial);
            Known& mesh = walk().adoptMesh(*part.mDrawable, model.readingOf(part), material.mIndex);

            held.mParts.push_back(AdoptedPart{
                .mMesh = mesh.mIndex,
                .mMaterial = material.mIndex,
                .mMeshEntry = &mesh,
                .mMaterialEntry = material.mKey != nullptr ? walk().findMaterial(material.mKey) : nullptr,
            });
        }
    }

    void CellRing::adoptGround(const PreparedCell& cell, HeldCell& held)
    {
        const PreparedGround& ground = cell.mGround;
        if (!ground.mStands)
            return;

        HeldGround& stands = held.mGround.emplace();
        mLayerScratch.clear();
        for (const PreparedLayer& layer : ground.mLayers)
        {
            MaterialLayer row;
            row.mDiffuse = mScene.addTexture(layer.mTexture->mPath);
            row.mDiffuseTransform = layer.mDiffuseTransform;

            if (!layer.mWeights.empty())
            {
                row.mMask = mScene.addMask(layer.mWeights.in(std::span<const float>(ground.mWeights)));
                row.mMaskWidth = layer.mMaskWidth;
                row.mMaskHeight = layer.mMaskHeight;
                row.mMaskTransform = layer.mMaskTransform;
            }

            mLayerScratch.push_back(row);

            stands.mTextures.push_back(layer.mTexture);
            holdTexture(*layer.mTexture);
        }

        stands.mLayers = static_cast<std::uint32_t>(mLayerScratch.size());
        stands.mOrigin = ground.mOrigin;
        stands.mFlattened = wantsFlattening(held.mCell, stands);

        // **The material before the mesh**, here too: a mesh records the material it arrives
        // wearing, and a cell's ground wears one for its life.
        Material material;
        material.mKind = MaterialKind::Terrain;
        material.mFlatten = stands.mFlattened;
        if (!mLayerScratch.empty())
            material.mLayers = mScene.addLayers(mLayerScratch);
        stands.mMaterial = mScene.addMaterial(material);

        // A heightfield is neither a sheet nor closed, and no fold is needed to say so.
        stands.mMesh = mScene.addMesh(ground.mPositions, ground.mNormals, ground.mTexCoords, ground.mIndices,
            FoldedShape{}, Deform::None, sNoIndex, stands.mMaterial);

        ++mCount.mMeshesAdded;
        ++mCount.mMaterialsAdded;
    }

    void CellRing::adopt(PreparedCell& cell)
    {
        HeldCell held;
        if (!mSpareCells.empty())
        {
            held = std::move(mSpareCells.back());
            mSpareCells.pop_back();
        }

        held.mCell = cell.mCell;
        held.mStatics = cell.mStatics;
        held.mPlacements.clear();
        held.mModels.clear();

        // **Emptied and kept, not reset**, so the texture list a spare cell grew is room the next
        // one refills rather than a heap call on the frame a cell lands.
        if (held.mGround.has_value())
            held.mGround->reuse();

        adoptGround(cell, held);

        for (PreparedModel* model : cell.mModels)
        {
            HeldModel& known = knownOf(*model);
            if (known.mParts.empty())
                adoptParts(known);

            ++known.mHeld;
            --known.mPending;
            held.mModels.push_back(model);
        }

        for (const PreparedRef& ref : cell.mRefs)
        {
            const PreparedModel& model = *cell.mModels[ref.mModel];
            const HeldModel& adopted = knownOf(model);

            for (std::size_t at = 0; at < adopted.mParts.size(); ++at)
                held.mPlacements.push_back(Placement{
                    .mMesh = adopted.mParts[at].mMesh,
                    .mMaterial = adopted.mParts[at].mMaterial,
                    .mTransform = model.mParts[at].mLocal * ref.mTransform,
                    .mRadius = ref.mRadius,
                    .mRefNum = ref.mRefNum,
                });
        }

        const auto at = std::lower_bound(mCells.begin(), mCells.end(), held.mCell,
            [](const HeldCell& left, const osg::Vec2i& right) { return cellKey(left) < cellKey(right); });
        mCells.insert(at, std::move(held));

        mSupply.giveBack().mCells.push_back(&cell);
    }

    void CellRing::discard(PreparedCell& cell)
    {
        for (PreparedModel* model : cell.mModels)
            release(*model, false);

        // Every hold the reader counted for the cell goes back with it: the models, and the
        // images its ground names.
        CellReturns& back = mSupply.giveBack();
        for (const PreparedLayer& layer : cell.mGround.mLayers)
            back.mTextures.push_back(layer.mTexture);
        giveBackHolds(cell.mModels, {});

        back.mCells.push_back(&cell);
    }

    void CellRing::dropSlots(HeldCell& cell)
    {
        for (Placement& placement : cell.mPlacements)
            if (placement.mSlot != sNoIndex)
            {
                mScene.dropInstance(placement.mSlot);
                placement.mSlot = sNoIndex;
                --mPlaced;
            }

        if (cell.mGround.has_value() && cell.mGround->mSlot != sNoIndex)
        {
            mScene.dropInstance(cell.mGround->mSlot);
            cell.mGround->mSlot = sNoIndex;
            --mGroundPlaced;
        }
    }

    void CellRing::dropCell(HeldCell& cell)
    {
        dropSlots(cell);

        for (PreparedModel* model : cell.mModels)
            release(*model, true);

        const std::span<PreparedTexture* const> textures = cell.mGround.has_value()
            ? std::span<PreparedTexture* const>(cell.mGround->mTextures)
            : std::span<PreparedTexture* const>();

        for (PreparedTexture* texture : textures)
            dropTexture(*texture);

        giveBackHolds(cell.mModels, textures);

        // The ground's rows are simply not named on this walk, and the sweep after it is told
        // there is something to release.
        if (cell.mGround.has_value())
        {
            ++mCount.mMeshesDisowned;
            ++mCount.mMaterialsDisowned;
            cell.mGround->reuse();
        }

        cell.mPlacements.clear();
        cell.mModels.clear();
        mSpareCells.push_back(std::move(cell));
    }

    void CellRing::dropPlacements()
    {
        for (HeldCell& cell : mCells)
            dropSlots(cell);
    }

    void CellRing::place(const osg::Vec2i& eye, const int reach)
    {
        for (HeldCell& cell : mCells)
        {
            const bool inReach = withinBand(cell.mCell, eye, reach);

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
                else if (!inReach && ground.mSlot != sNoIndex)
                {
                    mScene.dropInstance(ground.mSlot);
                    ground.mSlot = sNoIndex;
                    --mGroundPlaced;
                }

                // A cell crossing the grid's edge shades the other way from now on. The composite
                // it held goes with the rewrite, and one it now wants is asked for by the row.
                if (wantsFlattening(cell.mCell, ground) != ground.mFlattened)
                {
                    Material given = mScene.getTables().mMaterials.getRows()[ground.mMaterial];
                    given.mFlatten = !ground.mFlattened;
                    given.mDiffuse = sNoIndex;
                    mScene.setMaterial(ground.mMaterial, given);
                    ground.mFlattened = given.mFlatten;
                }
            }

            const bool shown = inReach && !inActiveGrid(cell.mCell);

            // The paging's own rule, per reference and per frame: a reference is placed while its
            // scaled radius clears the size threshold at the eye's distance to its cell.
            const float threshold = shown ? mMinSize * distanceTo(cell.mCell, mAround.mEye) : 0.0f;
            const float threshold2 = threshold * threshold;

            for (Placement& placement : cell.mPlacements)
            {
                const bool wanted
                    = shown && placement.mRadius * placement.mRadius >= threshold2 && !isDisabled(placement.mRefNum);

                if (wanted && placement.mSlot == sNoIndex)
                {
                    placement.mSlot = mScene.addInstance(MeshInstance{
                        .mTransform = placement.mTransform,
                        .mMesh = placement.mMesh,
                        .mMaterial = placement.mMaterial,
                    });
                    ++mPlaced;
                }
                else if (!wanted && placement.mSlot != sNoIndex)
                {
                    mScene.dropInstance(placement.mSlot);
                    placement.mSlot = sNoIndex;
                    --mPlaced;
                }
            }
        }
    }

    void CellRing::stamp()
    {
        for (const HeldModel& held : mModels)
            for (const AdoptedPart& part : held.mParts)
            {
                walk().keepMesh(*part.mMeshEntry);
                if (part.mMaterialEntry != nullptr)
                    walk().keepMaterial(*part.mMaterialEntry);
            }

        for (const HeldCell& cell : mCells)
            if (cell.mGround.has_value())
            {
                walk().keepOwnedMesh(cell.mGround->mMesh);
                walk().keepOwnedMaterial(cell.mGround->mMaterial);
            }
    }

    Collector& CellRing::walk() const
    {
        assert(mInto != nullptr && "the ring reached the walk from outside `collect`");
        return *mInto;
    }

    ResidencyCount CellRing::collect(Collector& into)
    {
        mInto = &into;

        // **Reported whatever else happens**, because a world with no reader is a world this has
        // just let go of: the rows it owned are gone, and only this says so.
        mCount.mDistantStatics = 0;
        mCount.mGroundCells = 0;

        if (!mSupply.hasReader())
            return report();

        takeDone();

        // **Indoors the eye's coordinates belong to another space**, so the rings are not moved:
        // what is held stays held for the way back out, and nothing stands.
        if (!mAround.mOutdoors)
        {
            dropPlacements();
            stamp();
            mSupply.publish();
            return report();
        }

        const osg::Vec2i eye = cellOf(mAround.mEye);
        const int reach = reachInCells();
        const int band = reach + sPreparedBand;

        // A cell held with the statics the other way is dropped whole and read again, for the
        // reason `takeDone` gives.
        for (auto cell = mCells.begin(); cell != mCells.end();)
        {
            if (withinBand(cell->mCell, eye, band) && cell->mStatics == mStatics)
            {
                ++cell;
                continue;
            }

            dropCell(*cell);
            cell = mCells.erase(cell);
        }

        for (auto cell = mPending.begin(); cell != mPending.end();)
        {
            if (withinBand((*cell)->mCell, eye, band) && !holds((*cell)->mCell))
            {
                ++cell;
                continue;
            }

            discard(**cell);
            cell = mPending.erase(cell);
        }

        ask(eye, band);

        // **Waited for after the ask that names it and never before**, because what the reader is
        // about to hand back is what that ask asked for. Nothing was asked for where the band is
        // whole, and then there is nothing to wait for.
        if (mSettled && mPending.empty() && !mAsking.mCells.empty())
            waitForNext();

        adoptPending();
        place(eye, reach);
        stamp();
        mSupply.publish();

        mCount.mDistantStatics = mPlaced;
        mCount.mGroundCells = mGroundPlaced;

        return report();
    }

    ResidencyCount CellRing::report()
    {
        // So that a reach from outside a walk fails where it is rather than counting into one that
        // has gone.
        mInto = nullptr;

        // **Spent here, because every field of it is told once.** A row disowned outside a walk has
        // to survive to the next one, and a walk that reported it twice would have the sweep free a
        // row that was already gone.
        return std::exchange(mCount, ResidencyCount{});
    }
}

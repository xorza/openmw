#include "cellring.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <span>
#include <utility>

#include <components/misc/constants.hpp>

#include "cellreader.hpp"
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

        MeshReading readingOf(const PreparedModel& model, const PreparedPart& part)
        {
            return MeshReading{
                .mPositions = std::span(model.mPositions).subspan(part.mFirstVertex, part.mVertexCount),
                .mNormals = std::span(model.mNormals).subspan(part.mFirstNormal, part.mNormalCount),
                .mTexCoords = std::span(model.mTexCoords).subspan(part.mFirstTexCoord, part.mTexCoordCount),
                .mIndices = std::span(model.mIndices).subspan(part.mFirstIndex, part.mIndexCount),
                .mShape = part.mShape,
            };
        }
    }

    CellRing::CellRing(SceneExtractor& extractor, SceneDesc& scene)
        : mExtractor(extractor)
        , mScene(scene)
    {
    }

    CellRing::~CellRing()
    {
        mWorker.request_stop();
        mWake.notify_all();
    }

    void CellRing::follow(const Terrain::ObjectStorage* const storage, Terrain::Storage* const ground,
        ContentSource* const content, const ESM::RefId worldspace, const osg::Node::NodeMask mask)
    {
        if (mStorage == storage && mGround == ground && mContent == content && mWorldspace == worldspace
            && mMask == mask)
            return;

        // **The thread reads the storages and the content without the lock**, which is sound only
        // because it is stopped and joined here before any is replaced — and before the reader
        // that holds them goes. Nothing is given back: what the frame held dies with the reader.
        mWorker.request_stop();
        mWake.notify_all();
        mWorker = {};

        std::uint32_t grounds = 0;
        for (HeldCell& cell : mCells)
        {
            dropSlots(cell);
            grounds += cell.mGroundMesh != sNoIndex ? 1 : 0;
        }
        mExtractor.disownRows(grounds, grounds);

        mCells.clear();
        mModels.clear();
        mTextures.clear();
        mPending.clear();
        mDone.clear();
        mWanted.clear();
        mRequested.clear();
        mReturnedCells.clear();
        mReturnedModels.clear();
        mReturnedTextures.clear();
        mReturnCellsScratch.clear();
        mReturnModelsScratch.clear();
        mReturnTexturesScratch.clear();
        mReader.reset();

        mStorage = storage;
        mGround = ground;
        mContent = content;
        mWorldspace = worldspace;
        mMask = mask;

        if (mStorage == nullptr || mGround == nullptr || mContent == nullptr)
            return;

        mReader = std::make_unique<CellReader>(*mStorage, *mGround, *mContent, mWorldspace, mMask);
        mWorker = std::jthread([this](std::stop_token stop) { work(stop); });
    }

    void CellRing::setStaticsEnabled(const bool enabled)
    {
        mStatics = enabled;
    }

    void CellRing::setReach(const float units)
    {
        mReach = units;
    }

    void CellRing::setMinSize(const float minSize)
    {
        mMinSize = minSize;
    }

    void CellRing::setActiveGrid(const osg::Vec4i& grid)
    {
        mActiveGrid = grid;
    }

    void CellRing::setViewPoint(const osg::Vec3f& viewPoint)
    {
        mViewPoint = viewPoint;
    }

    void CellRing::setOutdoors(const bool outdoors)
    {
        mOutdoors = outdoors;
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

    float CellRing::distanceTo(const osg::Vec2i& cell, const osg::Vec3f& eye)
    {
        const float low = static_cast<float>(cell.x()) * sCellSize;
        const float high = low + sCellSize;
        const float lowY = static_cast<float>(cell.y()) * sCellSize;
        const float highY = lowY + sCellSize;

        const float alongX = std::max({ low - eye.x(), eye.x() - high, 0.0f });
        const float alongY = std::max({ lowY - eye.y(), eye.y() - highY, 0.0f });

        return std::max(alongX, alongY);
    }

    bool CellRing::withinBand(const osg::Vec2i& cell, const osg::Vec2i& eye, const int band)
    {
        return std::abs(cell.x() - eye.x()) <= band && std::abs(cell.y() - eye.y()) <= band;
    }

    bool CellRing::inActiveGrid(const osg::Vec2i& cell) const
    {
        return cell.x() >= mActiveGrid.x() && cell.y() >= mActiveGrid.y() && cell.x() < mActiveGrid.z()
            && cell.y() < mActiveGrid.w();
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
        return static_cast<int>(std::ceil(mReach / sCellSize));
    }

    bool CellRing::wantsFlattening(const HeldCell& cell) const
    {
        // **A stack is flattened outside the active grid, and a single layer is never.** Inside
        // the grid the ground is near enough that the sharpness of a live stack is worth its cost
        // per hit, which is the rule the quad tree reached at about a cell out; a single layer is
        // already a single fetch, and flattening one would only resample a tiling texture into
        // something coarser than its file.
        return cell.mLayers > 1 && !inActiveGrid(cell.mCell);
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
        mReturnModelsScratch.insert(mReturnModelsScratch.end(), models.begin(), models.end());
        mReturnTexturesScratch.insert(mReturnTexturesScratch.end(), textures.begin(), textures.end());
    }

    void CellRing::takeDone()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mDoneScratch.swap(mDone);
        }

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
        mMissingScratch.clear();
        for (int x = eye.x() - band; x <= eye.x() + band; ++x)
            for (int y = eye.y() - band; y <= eye.y() + band; ++y)
            {
                const osg::Vec2i cell(x, y);
                if (!holds(cell) && !pending(cell))
                    mMissingScratch.push_back(cell);
            }

        std::sort(mMissingScratch.begin(), mMissingScratch.end(), Nearer{ eye });

        if (mMissingScratch == mRequested && mStatics == mRequestedStatics)
            return;

        mRequested = mMissingScratch;
        mRequestedStatics = mStatics;

        {
            std::lock_guard<std::mutex> lock(mMutex);
            mWanted = mMissingScratch;
            mWantedStatics = mStatics;
        }

        mWake.notify_one();
    }

    void CellRing::waitForWanted(const osg::Vec2i& eye, const int band)
    {
        for (;;)
        {
            takeDone();

            bool lacking = false;
            for (int x = eye.x() - band; x <= eye.x() + band && !lacking; ++x)
                for (int y = eye.y() - band; y <= eye.y() + band && !lacking; ++y)
                    lacking = !holds(osg::Vec2i(x, y)) && !pending(osg::Vec2i(x, y));

            if (!lacking)
                return;

            std::unique_lock<std::mutex> lock(mMutex);
            mDoneWake.wait(lock, [&] { return !mDone.empty(); });
        }
    }

    void CellRing::adoptPending()
    {
        if (mPending.empty())
            return;

        if (mSettled)
        {
            for (PreparedCell* cell : mPending)
                adopt(*cell);
            mPending.clear();
            return;
        }

        // **One cell a frame, and one frame walked twice adopts once.** A cell's meshes are copied
        // into the scene and its structures built by the hand-over that follows; two on one frame
        // would be the batch behind a threshold this renderer never takes.
        if (mAdoptedFrame == mFrame)
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
            const MaterialResolver::Resolved material = mExtractor.adoptMaterial(part.mMaterial);
            Known& mesh = mExtractor.adoptMesh(*part.mDrawable, readingOf(model, part), material.mIndex);

            held.mParts.push_back(AdoptedPart{
                .mMesh = mesh.mIndex,
                .mMaterial = material.mIndex,
                .mMeshEntry = &mesh,
                .mMaterialEntry = material.mKey != nullptr ? mExtractor.findMaterial(material.mKey) : nullptr,
            });
        }
    }

    void CellRing::adoptGround(const PreparedCell& cell, HeldCell& held)
    {
        const PreparedGround& ground = cell.mGround;
        if (!ground.mStands)
            return;

        mLayerScratch.clear();
        for (const PreparedLayer& layer : ground.mLayers)
        {
            MaterialLayer row;
            row.mDiffuse = mScene.addTexture(layer.mTexture->mPath);
            row.mDiffuseTransform = layer.mDiffuseTransform;

            if (layer.mWeightCount > 0)
            {
                row.mMask = mScene.addMask(
                    std::span<const float>(ground.mWeights).subspan(layer.mFirstWeight, layer.mWeightCount));
                row.mMaskWidth = layer.mMaskWidth;
                row.mMaskHeight = layer.mMaskHeight;
                row.mMaskTransform = layer.mMaskTransform;
            }

            mLayerScratch.push_back(row);

            held.mGroundTextures.push_back(layer.mTexture);
            holdTexture(*layer.mTexture);
        }

        held.mLayers = static_cast<std::uint32_t>(mLayerScratch.size());
        held.mGroundOrigin = ground.mOrigin;
        held.mFlattened = wantsFlattening(held);

        // **The material before the mesh**, here too: a mesh records the material it arrives
        // wearing, and a cell's ground wears one for its life.
        Material material;
        material.mKind = MaterialKind::Terrain;
        material.mFlatten = held.mFlattened;
        if (!mLayerScratch.empty())
            material.mLayers = mScene.addLayers(mLayerScratch);
        held.mGroundMaterial = mScene.addMaterial(material);

        // A heightfield is neither a sheet nor closed, and no fold is needed to say so.
        held.mGroundMesh = mScene.addMesh(ground.mPositions, ground.mNormals, ground.mTexCoords, ground.mIndices,
            FoldedShape{}, Deform::None, sNoIndex, held.mGroundMaterial);

        mExtractor.countOwnedRows(1, 1);
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
        held.mGroundMesh = sNoIndex;
        held.mGroundMaterial = sNoIndex;
        held.mGroundSlot = sNoIndex;
        held.mLayers = 0;
        held.mFlattened = false;
        held.mGroundTextures.clear();

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

        mReturnCellsScratch.push_back(&cell);
    }

    void CellRing::discard(PreparedCell& cell)
    {
        for (PreparedModel* model : cell.mModels)
            release(*model, false);

        // Every hold the reader counted for the cell goes back with it: the models, and the
        // images its ground names.
        for (const PreparedLayer& layer : cell.mGround.mLayers)
            mReturnTexturesScratch.push_back(layer.mTexture);
        giveBackHolds(cell.mModels, {});

        mReturnCellsScratch.push_back(&cell);
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

        if (cell.mGroundSlot != sNoIndex)
        {
            mScene.dropInstance(cell.mGroundSlot);
            cell.mGroundSlot = sNoIndex;
            --mGroundPlaced;
        }
    }

    void CellRing::dropCell(HeldCell& cell)
    {
        dropSlots(cell);

        for (PreparedModel* model : cell.mModels)
            release(*model, true);

        for (PreparedTexture* texture : cell.mGroundTextures)
            dropTexture(*texture);

        giveBackHolds(cell.mModels, cell.mGroundTextures);

        // The ground's rows are simply not named on this walk, and the sweep after it is told
        // there is something to release.
        if (cell.mGroundMesh != sNoIndex)
            mExtractor.disownRows(1, 1);

        cell.mPlacements.clear();
        cell.mModels.clear();
        cell.mGroundTextures.clear();
        cell.mGroundMesh = sNoIndex;
        cell.mGroundMaterial = sNoIndex;
        mSpareCells.push_back(std::move(cell));
    }

    void CellRing::dropPlacements()
    {
        for (HeldCell& cell : mCells)
            dropSlots(cell);
    }

    void CellRing::publishReturns()
    {
        if (mReturnCellsScratch.empty() && mReturnModelsScratch.empty() && mReturnTexturesScratch.empty())
            return;

        {
            std::lock_guard<std::mutex> lock(mMutex);
            mReturnedCells.insert(mReturnedCells.end(), mReturnCellsScratch.begin(), mReturnCellsScratch.end());
            mReturnedModels.insert(mReturnedModels.end(), mReturnModelsScratch.begin(), mReturnModelsScratch.end());
            mReturnedTextures.insert(
                mReturnedTextures.end(), mReturnTexturesScratch.begin(), mReturnTexturesScratch.end());
        }

        mReturnCellsScratch.clear();
        mReturnModelsScratch.clear();
        mReturnTexturesScratch.clear();
        mWake.notify_one();
    }

    void CellRing::place(const osg::Vec2i& eye, const int reach)
    {
        for (HeldCell& cell : mCells)
        {
            const bool inReach = withinBand(cell.mCell, eye, reach);

            // **The ground stands inside the active grid too**: the game builds none for this
            // renderer, so what a cell's land says is stood here wherever the cell is.
            if (cell.mGroundMesh != sNoIndex)
            {
                if (inReach && cell.mGroundSlot == sNoIndex)
                {
                    cell.mGroundSlot = mScene.addInstance(MeshInstance{
                        .mTransform = osg::Matrixf::translate(cell.mGroundOrigin),
                        .mMesh = cell.mGroundMesh,
                        .mMaterial = cell.mGroundMaterial,
                    });
                    ++mGroundPlaced;
                }
                else if (!inReach && cell.mGroundSlot != sNoIndex)
                {
                    mScene.dropInstance(cell.mGroundSlot);
                    cell.mGroundSlot = sNoIndex;
                    --mGroundPlaced;
                }

                // A cell crossing the grid's edge shades the other way from now on. The composite
                // it held goes with the rewrite, and one it now wants is asked for by the row.
                if (wantsFlattening(cell) != cell.mFlattened)
                {
                    Material given = mScene.getTables().mMaterials.getRows()[cell.mGroundMaterial];
                    given.mFlatten = !cell.mFlattened;
                    given.mDiffuse = sNoIndex;
                    mScene.setMaterial(cell.mGroundMaterial, given);
                    cell.mFlattened = given.mFlatten;
                }
            }

            const bool shown = inReach && !inActiveGrid(cell.mCell);

            // The paging's own rule, per reference and per frame: a reference is placed while its
            // scaled radius clears the size threshold at the eye's distance to its cell.
            const float threshold = shown ? mMinSize * distanceTo(cell.mCell, mViewPoint) : 0.0f;
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
                mExtractor.keepMesh(*part.mMeshEntry);
                if (part.mMaterialEntry != nullptr)
                    mExtractor.keepMaterial(*part.mMaterialEntry);
            }

        for (const HeldCell& cell : mCells)
            if (cell.mGroundMesh != sNoIndex)
            {
                mExtractor.keepOwnedMesh(cell.mGroundMesh);
                mExtractor.keepOwnedMaterial(cell.mGroundMaterial);
            }
    }

    void CellRing::collect(Collector&)
    {
        if (mReader == nullptr)
            return;

        takeDone();

        // **Indoors the eye's coordinates belong to another space**, so the rings are not moved:
        // what is held stays held for the way back out, and nothing stands.
        if (!mOutdoors)
        {
            dropPlacements();
            stamp();
            publishReturns();
            return;
        }

        const osg::Vec2i eye = cellOf(mViewPoint);
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

        if (mSettled)
            waitForWanted(eye, band);

        adoptPending();
        place(eye, reach);
        stamp();
        publishReturns();

        mExtractor.countDistantStatics(mPlaced);
        mExtractor.countGround(mGroundPlaced);
    }

    void CellRing::recycle()
    {
        for (PreparedCell* cell : mReturnedCells)
            mReader->giveBack(*cell);
        mReturnedCells.clear();

        for (PreparedTexture* texture : mReturnedTextures)
            mReader->giveBack(*texture);
        mReturnedTextures.clear();

        for (PreparedModel* model : mReturnedModels)
            mReader->giveBack(*model);
        mReturnedModels.clear();
    }

    void CellRing::work(std::stop_token stop)
    {
        std::unique_lock<std::mutex> lock(mMutex);
        while (mWake.wait(lock, stop, [&] {
            return !mWanted.empty() || !mReturnedCells.empty() || !mReturnedModels.empty()
                || !mReturnedTextures.empty();
        }))
        {
            if (stop.stop_requested())
                return;

            recycle();

            mRequest.swap(mWanted);
            mWanted.clear();
            mRequestStatics = mWantedStatics;
            lock.unlock();

            for (const osg::Vec2i& cell : mRequest)
            {
                if (stop.stop_requested())
                    break;

                // A newer list replaces this one: the eye has moved and what it lacks has changed.
                lock.lock();
                const bool newer = !mWanted.empty();
                recycle();
                lock.unlock();

                if (newer)
                    break;

                PreparedCell& made = mReader->read(cell, mRequestStatics);

                lock.lock();
                mDone.push_back(&made);
                lock.unlock();
                mDoneWake.notify_all();
            }

            lock.lock();
        }
    }
}

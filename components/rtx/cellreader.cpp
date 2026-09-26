#include "cellreader.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Matrixf>
#include <osg/Quat>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/misc/resourcehelpers.hpp>
#include <components/sceneutil/lightcommon.hpp>

#include "cellworld.hpp"
#include "contract.hpp"
#include "lightbuilder.hpp"
#include "prepared.hpp"
#include "refusals.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        osg::Matrixf transformOf(const Terrain::PagedCellRef& ref)
        {
            const osg::Quat attitude = osg::Quat(ref.mRotation.z(), osg::Vec3f(0.0f, 0.0f, -1.0f))
                * osg::Quat(ref.mRotation.y(), osg::Vec3f(0.0f, -1.0f, 0.0f))
                * osg::Quat(ref.mRotation.x(), osg::Vec3f(-1.0f, 0.0f, 0.0f));

            osg::Matrixf transform;
            transform.preMultTranslate(ref.mPosition);
            transform.preMultRotate(attitude);
            transform.preMultScale(osg::Vec3f(ref.mScale, ref.mScale, ref.mScale));

            return transform;
        }

    }

    CellReader::CellReader(const Terrain::ObjectStorage& storage, Terrain::Storage& ground, ContentSource& content,
        const ESM::RefId worldspace, const osg::Node::NodeMask mask)
        : mStorage(storage)
        , mContent(content)
        , mWorldspace(worldspace)
        , mMask(mask)
        , mGround(ground, content, worldspace)
        , mCollector(storage.makeCollector())
    {
    }

    PreparedTexture& CellReader::readTexture(
        const osg::ref_ptr<const osg::Image>& image, const VFS::Path::Normalized& path)
    {
        if (const auto known = mTexturesByPath.find(path.value()); known != mTexturesByPath.end())
        {
            mTextures.lend(**known);
            return **known;
        }

        PreparedTexture& texture = mTextures.take([&](PreparedTexture& into) {
            into.mImage = image;
            into.mPath = path;
        });

        mTextures.lend(texture);
        [[maybe_unused]] const bool fresh = mTexturesByPath.insert(&texture).second;
        assert(fresh && "a texture filed twice");

        return texture;
    }

    PreparedModel* CellReader::readModel(const VFS::Path::NormalizedView path)
    {
        if (const auto known = mModelsByPath.find(path.value()); known != mModelsByPath.end())
            return *known;

        const osg::ref_ptr<const osg::Node> node = mContent.getTemplate(path);
        if (node == nullptr)
            return nullptr;

        PreparedModel& model = mModels.take([&](PreparedModel& into) {
            into.mPath.assign(path.value());
            into.mTemplate = node;

            // The template's own bound, as `createChunk` measured a reference by it. Computed at
            // load for every template the game hands out, so this is a read.
            into.mRadius = node->getBound().radius();

            // What the walk had read before it met what it cannot take goes, and the reason stays.
            // The loader answers a file it cannot read with the error marker, which reads.
            const Result<void, std::string> walked = mWalk.read(*node, mMask, into);
            if (!walked.isOk())
            {
                into.reuse();
                into.mPath.assign(path.value());
                into.mRefused.assign(walked.error());
            }
        });

        [[maybe_unused]] const bool fresh = mModelsByPath.insert(&model).second;
        assert(fresh && "a model filed twice");

        return &model;
    }

    PreparedCell& CellReader::read(const osg::Vec2i& cell, const bool statics)
    {
        PreparedCell& prepared = mCells.take([&](PreparedCell& into) { fill(into, cell, statics); });
        mCells.lend(prepared);
        return prepared;
    }

    void CellReader::fill(PreparedCell& prepared, const osg::Vec2i& cell, const bool statics)
    {
        prepared.mCell = cell;
        prepared.mStatics = statics;

        mGround.read(cell, prepared.mGround);

        // Each layer's texture counted once for the cell, so the reading stands until the frame
        // gives the cell's hold on it back.
        const std::span<const GroundReader::LayerFiles> files = mGround.getLayerFiles();
        assert(files.size() == prepared.mGround.mLayers.size() && "a layer the ground reader found no files for");
        for (std::size_t at = 0; at < files.size(); ++at)
        {
            PreparedLayer& layer = prepared.mGround.mLayers[at];
            layer.mTexture = &readTexture(files[at].mImage, files[at].mPath);
            if (!files[at].mNormalPath.empty())
                layer.mNormalTexture = &readTexture(files[at].mNormalImage, files[at].mNormalPath);
        }

        // One cell at a time, which is the paging's near answer: containers page here as they do
        // in the active grid's own chunks, and the size rule is what thins them with distance. One
        // walk of the cell's records answers the lights too, which the paging never stands, and a
        // reference is a lamp where its record is a `LIGH`: the paging draws no lamp's mesh in the
        // distance, so neither is one stood here.
        mStorage.collect(1.0f, cell, mWorldspace, Terrain::RefKinds::Both, *mCollector, mRefScratch);

        // Which references are lamps, asked once per reference: the statics below skip them.
        mIsLampScratch.assign(mRefScratch.size(), 0);
        for (std::size_t at = 0; at < mRefScratch.size(); ++at)
        {
            const Terrain::PagedCellRef& ref = mRefScratch[at];
            const std::optional<SceneUtil::LightCommon> record = mStorage.getLight(ref.mRefId);
            if (!record.has_value())
                continue;

            mIsLampScratch[at] = 1;

            // Made once here to be judged, and again every walk to be stood, because a flame is a
            // function of the hour and whether a lamp is refused is not. A record off by default
            // casts nothing wherever it is placed, so it is not carried.
            const Result<std::optional<Light>, std::string_view> made
                = makeLight(*record, ref.mPosition, 0.0, static_cast<int>(ref.mRefNum.mIndex));
            if (!made.isOk())
                prepared.mRefusals.push_back(Refusal{
                    .mKind = Refused::Lamp, .mName = ref.mRefId.toDebugString(), .mWhy = std::string(made.error()) });
            else if (made.value().has_value())
                prepared.mLights.push_back(PreparedLight{
                    .mPosition = ref.mPosition,
                    .mRefNum = ref.mRefNum,
                    .mRecord = *record,
                });
        }

        if (!statics)
            return;

        for (std::size_t at = 0; at < mRefScratch.size(); ++at)
        {
            const Terrain::PagedCellRef& ref = mRefScratch[at];

            // A lamp, carried above; a reference naming no record is the content's to answer for,
            // and the game draws nothing for one either.
            if (mIsLampScratch[at] != 0 || Misc::ResourceHelpers::isHiddenMarker(ref.mRefId))
                continue;

            // The path a record names, built once for the record and not once for every
            // reference to it: a town is a few hundred references to a few dozen models.
            auto named = mModelPaths.find(ref.mRefId);
            if (named == mModelPaths.end())
            {
                VFS::Path::Normalized model = mStorage.getModel(ref.mRefId);
                if (!model.empty())
                    model = Misc::ResourceHelpers::correctMeshPath(model);
                named = mModelPaths.emplace(ref.mRefId, std::move(model)).first;
            }

            const VFS::Path::Normalized& model = named->second;
            if (model.empty())
                continue;

            // A model this cannot read is a reference left out and refused, and never a cell
            // left out: a settled walk waits for every cell of the ring, and one that never came
            // would hold it for ever.
            PreparedModel* read = readModel(model);
            if (read == nullptr)
                continue;

            if (!read->mRefused.empty())
            {
                prepared.mRefusals.push_back(
                    Refusal{ .mKind = Refused::Model, .mName = read->mPath, .mWhy = read->mRefused });
                continue;
            }

            if (read->mParts.empty())
                continue;

            std::uint32_t index = 0;
            for (; index < prepared.mModels.size(); ++index)
                if (prepared.mModels[index] == read)
                    break;

            // One hold for the cell, however many of its references stand the model.
            if (index == prepared.mModels.size())
            {
                prepared.mModels.push_back(read);
                mModels.lend(*read);
            }

            prepared.mRefs.push_back(PreparedRef{
                .mModel = index,
                .mRefNum = ref.mRefNum,
                .mTransform = transformOf(ref),
                .mRadius = read->mRadius * ref.mScale,
            });
        }
    }

    void CellReader::giveBack(PreparedCell& cell)
    {
        if (!mCells.release(cell))
            return;

        cell.reuse();
        mCells.give(cell);
    }

    void CellReader::giveBack(PreparedTexture& texture)
    {
        if (!mTextures.release(texture))
            return;

        // Erased under the path it is still filed under, before `reuse` clears it.
        const auto filed = mTexturesByPath.find(texture.mPath.value());
        contract(filed != mTexturesByPath.end(), "a texture given back that was never filed");
        mTexturesByPath.erase(filed);

        texture.reuse();
        mTextures.give(texture);
    }

    void CellReader::giveBack(PreparedModel& model)
    {
        if (!mModels.release(model))
            return;

        // Erased under the path it is still filed under, before `reuse` clears it.
        const auto filed = mModelsByPath.find(std::string_view(model.mPath));
        contract(filed != mModelsByPath.end(), "a model given back that was never filed");
        mModelsByPath.erase(filed);

        model.reuse();
        mModels.give(model);
    }
}

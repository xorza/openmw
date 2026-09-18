#include "cellreader.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

#include <osg/Matrixf>
#include <osg/Quat>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/debug/debuglog.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/sceneutil/lightcommon.hpp>

#include "error.hpp"
#include "lightbuilder.hpp"
#include "residency.hpp"

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
    {
    }

    PreparedTexture* CellReader::readTexture(const osg::Image& image)
    {
        if (image.getFileName().empty())
            return nullptr;

        if (PreparedTexture* const* const known = mByImage.find(&image))
        {
            mTextures.lend(**known);
            return *known;
        }

        PreparedTexture& texture = mTextures.take([&](PreparedTexture& into) {
            into.mImage = &image;
            into.mPath = VFS::Path::Normalized(image.getFileName());
        });

        mTextures.lend(texture);
        mByImage.insert(&texture);

        return &texture;
    }

    PreparedModel* CellReader::readModel(const VFS::Path::NormalizedView path)
    {
        if (PreparedModel* const* const known = mByPath.find(path.value()))
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

            mWalk.read(*node, mMask, into);
        });

        mByPath.insert(&model);

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

        // Each layer's image counted once for the cell, so the reading stands until the frame gives
        // the cell's hold on it back. A layer whose image names no file has nothing to describe
        // and no slot to take, and goes.
        std::erase_if(prepared.mGround.mLayers, [&](PreparedLayer& layer) {
            layer.mTexture = readTexture(*layer.mImage);
            return layer.mTexture == nullptr;
        });

        // One cell at a time, which is the paging's near answer: containers page here as they do
        // in the active grid's own chunks, and the size rule is what thins them with distance. One
        // walk of the cell's records answers the lights too, which the paging never stands.
        mStorage.collect(1.0f, cell, mWorldspace, mRefScratch, mLitScratch);

        for (const Terrain::PagedCellRef& ref : mLitScratch)
        {
            const std::optional<SceneUtil::LightCommon> record = mStorage.getLight(ref.mRefId);

            // A reference naming no record is the content's to answer for, and the game draws
            // nothing for one either. Nothing is invented here to stand in its place; and a record
            // off by default casts nothing wherever it is placed, so it is not carried.
            if (!record.has_value() || !castsWherePlaced(*record))
                continue;

            prepared.mLights.push_back(PreparedLight{
                .mPosition = ref.mPosition,
                .mRefNum = ref.mRefNum,
                .mRecord = *record,
            });
        }

        if (!statics)
            return;

        for (const Terrain::PagedCellRef& ref : mRefScratch)
        {
            if (Misc::ResourceHelpers::isHiddenMarker(ref.mRefId))
                continue;

            VFS::Path::Normalized model = mStorage.getModel(ref.mRefId);
            if (model.empty())
                continue;

            model = Misc::ResourceHelpers::correctMeshPath(model);

            // A model this cannot read is a reference left out and named, and never a cell
            // left out: a settled walk waits for every cell of the ring, and one that never came
            // would hold it for ever. `Error` and no wider: the walk is what throws it, for a
            // file that describes a mesh this renderer cannot take, and the loader answers a file
            // it cannot read with the error marker rather than a throw. Anything else is the
            // reader failing, which the monitor reports.
            PreparedModel* read = nullptr;
            try
            {
                read = readModel(model);
            }
            catch (const Error& e)
            {
                Log(Debug::Warning) << "Ray tracing could not read " << model << " for " << ref.mRefId << ": "
                                    << e.what();
                continue;
            }

            if (read == nullptr || read->mParts.empty())
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

        mByImage.erase(texture.mImage.get());
        texture.reuse();
        mTextures.give(texture);
    }

    void CellReader::giveBack(PreparedModel& model)
    {
        if (!mModels.release(model))
            return;

        // Erased under the path it is still filed under, before `reuse` clears it.
        mByPath.erase(std::string_view(model.mPath));

        model.reuse();
        mModels.give(model);
    }
}

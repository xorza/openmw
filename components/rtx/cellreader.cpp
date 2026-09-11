#include "cellreader.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <exception>
#include <span>
#include <utility>
#include <vector>

#include <osg/Drawable>
#include <osg/Matrixf>
#include <osg/Quat>
#include <osg/Vec3f>

#include <components/debug/debuglog.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/surface/material.hpp>

#include "contentsource.hpp"
#include "error.hpp"
#include "materialresolver.hpp"
#include "shading.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    namespace
    {
        /// Appends `values` to `into` and answers the run they landed in.
        ///
        /// **One statement, because four parts of it were the same three lines.** A run that named
        /// where it started and a count taken from another array is exactly what `Rtx::Run` exists
        /// to stop.
        template <class T>
        Run appended(std::vector<T>& into, std::span<const T> values)
        {
            const Run run{ .mOffset = static_cast<std::uint32_t>(into.size()),
                .mCount = static_cast<std::uint32_t>(values.size()) };
            into.insert(into.end(), values.begin(), values.end());

            return run;
        }

        /// Fills one model from the drawables a template walk hands over.
        class ModelTaker final : public TemplateSink
        {
        public:
            ModelTaker(PreparedModel& into, MeshReader& meshes, AlphaScratch& alpha, const NodeKinds& kinds)
                : mInto(into)
                , mMeshes(meshes)
                , mAlpha(alpha)
                , mKinds(kinds)
            {
            }

            void take(
                const osg::Drawable& drawable, std::span<const Shading> shading, const osg::Matrixf& local) override
            {
                // A particle system is a drawable with no triangles, and the paging left those out
                // of a chunk too; a rig or a morph is read as its source, which is the bind pose,
                // and stands still — as it did in a chunk.
                const DrawableRead read = readDrawable(drawable, mKinds.of(drawable));
                if (read.mGeometry == nullptr)
                    return;

                MeshReading reading;
                if (!mMeshes.read(read, reading))
                    return;

                PreparedPart part;
                part.mDrawable = &drawable;
                part.mMaterial = MaterialResolver::read(shading, mAlpha);
                part.mLocal = local;
                part.mShape = reading.mShape;

                part.mVertices = appended(mInto.mPositions, reading.mArrays.mPositions);
                part.mNormals = appended(mInto.mNormals, reading.mArrays.mNormals);
                part.mTexCoords = appended(mInto.mTexCoords, reading.mArrays.mTexCoords);
                part.mColours = appended(mInto.mColours, reading.mArrays.mColours);
                part.mIndices = appended(mInto.mIndices, reading.mArrays.mIndices);

                mInto.mParts.push_back(std::move(part));
            }

        private:
            PreparedModel& mInto;
            MeshReader& mMeshes;
            AlphaScratch& mAlpha;
            const NodeKinds& mKinds;
        };

        /// The reference's own space to the world's, composed as `SceneUtil::PositionAttitudeTransform`
        /// composes the one the game stands a clone under: scaled, then turned, then moved.
        ///
        /// **The paging's own quaternion**, axis by negated axis: Morrowind's angles turn the other
        /// way about each axis, and the order they are applied in — Z first — is the content's, so
        /// what stands here is what `createChunk` stood.
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

        /// The three images a material can name, in the roles the frame's describe takes them by.
        constexpr std::array<Surface::TextureRole, 4> sRoles{ Surface::TextureRole::Diffuse,
            Surface::TextureRole::Emissive, Surface::TextureRole::Normal, Surface::TextureRole::NormalHeight };

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
            ++(*known)->mLent;
            return *known;
        }

        PreparedTexture& texture = mTextures.take();
        texture.mImage = &image;
        texture.mPath = VFS::Path::Normalized(image.getFileName());
        texture.mLent = 1;

        // **What the frame's describe would have done, done here.** A file that carried no chain
        // gets one built; every file gets its shading estimated. Both read every texel, and both
        // are what the frame then finds ready. A format this renderer does not upload is recorded as
        // such and drawn as the stand-in there, as it would be without this.
        try
        {
            mLevelScratch.clear();
            TextureData described = describeImage(image, mLevelScratch);

            texture.mChain.build(described);
            if (!texture.mChain.isEmpty())
                described = texture.mChain.describe();

            const ShadingMap map(described);
            const std::span<const float> values = map.getValues();
            std::copy(values.begin(), values.end(), texture.mShading.begin());

            texture.mReadable = true;
        }
        catch (const Error&)
        {
            texture.mReadable = false;
        }

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

        PreparedModel& model = mModels.take();
        model.mPath.assign(path.value());
        model.mTemplate = node;

        // The template's own bound, as `createChunk` measured a reference by it. Computed at load
        // for every template the game hands out, so this is a read.
        model.mRadius = node->getBound().radius();

        ModelTaker taker(model, mMeshes, mAlpha, mKinds);
        mWalk.walk(*node, mMask, taker);

        for (const PreparedPart& part : model.mParts)
        {
            if (part.mMaterial.mDescribed == nullptr)
                continue;

            for (const Surface::TextureRole role : sRoles)
            {
                const osg::Image* const image = part.mMaterial.mDescribed->getTexture(role);
                if (image == nullptr)
                    continue;

                // Once per model, however many parts wear it: the holder count is of models.
                const bool named = std::any_of(model.mTextures.begin(), model.mTextures.end(),
                    [&](const PreparedTexture* held) { return held->mImage == image; });
                if (named)
                    continue;

                if (PreparedTexture* texture = readTexture(*image))
                    model.mTextures.push_back(texture);
            }
        }

        mByPath.insert(&model);

        return &model;
    }

    PreparedCell& CellReader::read(const osg::Vec2i& cell, const bool statics)
    {
        PreparedCell& prepared = mCells.take();
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

        if (!statics)
            return prepared;

        // One cell at a time, which is `wantedType`'s near answer: containers page here as they do
        // in the active grid's own chunks, and the size rule is what thins them with distance.
        mStorage.collect(Terrain::RefKind::Paged, 1.0f, cell, mWorldspace, mRefScratch);

        for (const Terrain::PagedCellRef& ref : mRefScratch)
        {
            if (Misc::ResourceHelpers::isHiddenMarker(ref.mRefId))
                continue;

            VFS::Path::Normalized model = mStorage.getModel(ref.mType, ref.mRefId);
            if (model.empty())
                continue;

            model = Misc::ResourceHelpers::correctMeshPath(model);

            // **A model this cannot read is a reference left out and named**, and never a cell
            // left out: a settled walk waits for every cell of the ring, and one that never came
            // would hold it for ever.
            PreparedModel* read = nullptr;
            try
            {
                read = readModel(model);
            }
            catch (const std::exception& e)
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
                ++read->mLent;
            }

            prepared.mRefs.push_back(PreparedRef{
                .mModel = index,
                .mRefNum = ref.mRefNum,
                .mTransform = transformOf(ref),
                .mRadius = read->mRadius * ref.mScale,
            });
        }

        return prepared;
    }

    void CellReader::giveBack(PreparedCell& cell)
    {
        cell.reuse();
        mCells.give(cell);
    }

    void CellReader::giveBack(PreparedTexture& texture)
    {
        assert(texture.mLent > 0 && "an image given back more often than it was lent");
        if (--texture.mLent > 0)
            return;

        mByImage.erase(texture.mImage.get());
        texture.reuse();
        mTextures.give(texture);
    }

    void CellReader::giveBack(PreparedModel& model)
    {
        assert(model.mLent > 0 && "a model given back more often than it was lent");
        if (--model.mLent > 0)
            return;

        for (PreparedTexture* texture : model.mTextures)
            giveBack(*texture);

        // Erased under the path it is still filed under, before `reuse` clears it.
        mByPath.erase(std::string_view(model.mPath));

        model.reuse();
        mModels.give(model);
    }
}

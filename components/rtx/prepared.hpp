#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <osg/Drawable>
#include <osg/Image>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Vec2f>
#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/esm3/refnum.hpp>
#include <components/vfs/pathutil.hpp>

#include "materialresolver.hpp"
#include "meshreader.hpp"
#include "mipchain.hpp"
#include "runs.hpp"
#include "scratch.hpp"
#include "shadingmap.hpp"
#include "shapefold.hpp"

namespace Rtx
{
    /// One image, described where the model that names it was read: the levels its file did not
    /// carry, and the light painted into it — the two things describing a texture costs, both of
    /// which read every texel, done off the frame. Lent by a `Spares` and given back.
    struct PreparedTexture
    {
        /// The image itself, which is what the frame's describe looks a texture up by: the loader's
        /// cache hands the same object to the template and to whoever asks for the path.
        osg::ref_ptr<const osg::Image> mImage;

        /// The image's file, normalised, which is what the scene names a texture by. Kept here so
        /// that a frame adopting a layer names its texture without building the path again.
        VFS::Path::Normalized mPath;

        /// The levels the file did not carry, or empty where it carried them.
        MipChain mChain;

        /// `ShadingMap::sCells` factors, row by row.
        std::array<float, ShadingMap::sCells> mShading{};

        /// False where the image is in a format this renderer does not upload, which the frame
        /// draws the stand-in for. Nothing above is meaningful then.
        bool mReadable = false;

        /// How many lent models and cells name it. The reader's count, and given back with the last
        /// of them — `PreparedModel::mLent` is the same count for a model.
        std::uint32_t mLent = 0;

        /// Makes room for the next image. The chain keeps its bytes.
        void reuse() { reuseKeeping(*this, &PreparedTexture::mPath, &PreparedTexture::mChain); }
    };

    /// One ground texture a cell's land names, and the weights that place it.
    struct PreparedLayer
    {
        /// The tiling ground texture, opened on the thread. What its reading is looked up by.
        osg::ref_ptr<const osg::Image> mImage;

        /// The reader's description of it, lent for as long as the cell is held.
        PreparedTexture* mTexture = nullptr;

        /// Into `PreparedGround::mWeights`. An empty run is a layer covering the whole cell.
        Run mWeights;
        std::uint16_t mMaskWidth = 0;
        std::uint16_t mMaskHeight = 0;

        /// Cell texture coordinates to this layer's, as `uv * xy + zw`. `GroundReader` says where
        /// the two come from.
        osg::Vec4f mDiffuseTransform{ 1.0f, 1.0f, 0.0f, 0.0f };
        osg::Vec4f mMaskTransform{ 1.0f, 1.0f, 0.0f, 0.0f };
    };

    /// One cell's ground, read off the land records on a thread that is not the frame's. A cell
    /// and not a chunk: the content's own 65 × 65 grid shares its edge rows with its neighbours, so
    /// two cells meet vertex for vertex whatever the eye does.
    struct PreparedGround
    {
        /// False where the land names no ground at all, which stands nothing.
        bool mStands = false;

        /// The cell's centre in world units, which the positions are relative to.
        osg::Vec3f mOrigin;

        std::vector<osg::Vec3f> mPositions;
        std::vector<osg::Vec3f> mNormals;

        /// The land's own per-vertex colour, in linear light — `VCLR`, painted over two thirds of
        /// Morrowind's exterior vertices. White for a cell with no land record.
        std::vector<osg::Vec3f> mColours;

        /// The reader's own, shared by every cell of one shape: a grid's corners and its
        /// triangulation are the same for every cell.
        std::span<const osg::Vec2f> mTexCoords;
        std::span<const std::uint32_t> mIndices;

        std::vector<PreparedLayer> mLayers;

        /// Every layer's weights end to end, row by row.
        std::vector<float> mWeights;

        /// Makes room for the next cell, keeping what the buffers grew.
        void reuse()
        {
            reuseKeeping(*this, &PreparedGround::mPositions, &PreparedGround::mNormals, &PreparedGround::mColours,
                &PreparedGround::mLayers, &PreparedGround::mWeights);
        }
    };

    /// One drawable of a prepared model: what the frame adopts it under, and where its triangles
    /// sit in the model's buffers.
    struct PreparedPart
    {
        /// The template's own drawable, which is the identity the frame's walk will find a clone's
        /// mesh under. Held, so the address stays true for as long as the reading does.
        osg::ref_ptr<const osg::Drawable> mDrawable;

        MaterialReading mMaterial;

        /// Where the part stands in the template's own space.
        osg::Matrixf mLocal;

        /// Where this part's attributes sit in the model's own buffers.
        Run mVertices;

        /// Empty where the geometry names no normal, no texture coordinate and no colour.
        Run mNormals;
        Run mTexCoords;
        Run mColours;

        Run mIndices;

        FoldedShape mShape;
    };

    /// A model read whole on a thread that is not the frame's: its parts, the folded geometry of
    /// every one of them in the format the scene copies from, and the images they name. Owned by
    /// the reader and lent to every cell that names it, once per cell; refilled for the next model
    /// once every hold is back. Flat, one buffer per attribute, because a cell names hundreds of
    /// models of a handful of parts each.
    struct PreparedModel
    {
        /// The corrected path the model was read under, which is what the reader finds it by.
        std::string mPath;

        /// Held, because every part's drawable and state set are the template's own.
        osg::ref_ptr<const osg::Node> mTemplate;

        /// The template's own bound, which is what the paging measured a reference by.
        float mRadius = 0.0f;

        /// How many cells the reader has lent this to and not yet had back. The reader's own, and
        /// the frame never reads it: `CellReader::giveBack` says why a hold is a cell's and not
        /// the frame's.
        std::uint32_t mLent = 0;

        std::vector<PreparedPart> mParts;

        /// Every image the parts name, once each. The reader's, lent for as long as this is.
        std::vector<PreparedTexture*> mTextures;

        std::vector<osg::Vec3f> mPositions;
        std::vector<osg::Vec3f> mNormals;
        std::vector<osg::Vec2f> mTexCoords;
        std::vector<osg::Vec3f> mColours;
        std::vector<std::uint32_t> mIndices;

        /// What one of its parts comes to, as the frame adopts it. The spans are into this model's
        /// own storage, and live for as long as it is lent.
        MeshReading readingOf(const PreparedPart& part) const
        {
            return MeshReading{
                .mArrays = {
                    .mPositions = part.mVertices.in(std::span<const osg::Vec3f>(mPositions)),
                    .mNormals = part.mNormals.in(std::span<const osg::Vec3f>(mNormals)),
                    .mTexCoords = part.mTexCoords.in(std::span<const osg::Vec2f>(mTexCoords)),
                    .mColours = part.mColours.in(std::span<const osg::Vec3f>(mColours)),
                    .mIndices = part.mIndices.in(std::span<const std::uint32_t>(mIndices)),
                },
                .mShape = part.mShape,
            };
        }

        /// Makes room for the next model, keeping what the buffers grew.
        void reuse()
        {
            reuseKeeping(*this, &PreparedModel::mPath, &PreparedModel::mParts, &PreparedModel::mTextures,
                &PreparedModel::mPositions, &PreparedModel::mNormals, &PreparedModel::mTexCoords,
                &PreparedModel::mColours, &PreparedModel::mIndices);
        }
    };

    /// One reference a prepared cell stands: which of the cell's models, where, and how big.
    struct PreparedRef
    {
        /// Into `PreparedCell::mModels`.
        std::uint32_t mModel = 0;

        ESM::RefNum mRefNum;

        /// The reference's own space to the world's — position, rotation and scale, composed as
        /// the game composes them for the transform it stands a clone under.
        osg::Matrixf mTransform;

        /// The model's radius at the reference's scale, which the size rule reads.
        float mRadius = 0.0f;
    };

    /// What a thread hands the frame for one cell: its ground, the references that page, reduced
    /// the way the content files stack, and the models they name.
    ///
    /// Owned by the reader and lent to the frame, as a model is.
    struct PreparedCell
    {
        osg::Vec2i mCell;

        /// Whether the references were read at all, which is the ring's statics switch as it stood
        /// when the cell was read.
        bool mStatics = false;

        PreparedGround mGround;

        /// Every model the references name, once each.
        std::vector<PreparedModel*> mModels;

        std::vector<PreparedRef> mRefs;

        void reuse() { reuseKeeping(*this, &PreparedCell::mGround, &PreparedCell::mModels, &PreparedCell::mRefs); }
    };
}

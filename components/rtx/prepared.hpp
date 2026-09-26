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
#include <components/sceneutil/lightcommon.hpp>
#include <components/vfs/pathutil.hpp>

#include "material.hpp"
#include "materialresolver.hpp"
#include "meshreader.hpp"
#include "refusals.hpp"
#include "runs.hpp"
#include "scratch.hpp"
#include "shapefold.hpp"

namespace Rtx
{
    /// One image, named where the model that names it was read, so that the frame adopts a layer
    /// by a path built off the frame. Lent by a `Spares`, held once by every lent model and cell
    /// that names it, and given back with the last of them. The reader's count, and the frame
    /// never reads it: `CellReader::giveBack` says why a hold is a cell's and not the frame's.
    struct PreparedTexture : Lent
    {
        /// The image itself, held so the loader's cache hands the same object to the frame's
        /// describe, or null where the file does not read.
        osg::ref_ptr<const osg::Image> mImage;

        /// The file, normalised, which is what the reader files it under and the scene names a
        /// texture by. Kept here so that a frame adopting a layer names its texture without
        /// building the path again.
        VFS::Path::Normalized mPath;

        /// Makes room for the next image.
        void reuse() { reuseKeeping(*this, &PreparedTexture::mPath); }
    };

    /// One ground texture a cell's land names, and the weights that place it.
    struct PreparedLayer
    {
        /// The reader's description of the tiling ground texture, lent for as long as the cell is
        /// held: its image, opened on the thread or null where it does not read — a layer the
        /// texture table stands in for — and the path the land names it by.
        PreparedTexture* mTexture = nullptr;

        /// The same for the layer's normal map as the storage found it beside the diffuse (`_nh`
        /// before `_n`), or null where there is none.
        PreparedTexture* mNormalTexture = nullptr;

        /// Whether that normal map's alpha is a height: an `_nh` the storage found, and
        /// `carriesHeight`.
        bool mParallax = false;

        /// Whether the storage swapped `_diffusespec` in for the diffuse. What its alpha means is
        /// the layout's to say, and the placer is what knows the layout.
        bool mDiffuseSpec = false;

        /// Into `PreparedGround::mWeights`. An empty run is a layer covering the whole cell.
        Run mWeights;

        /// The scene's row as far as the land record fills it — the grid and the two transforms —
        /// so adopting a layer copies one value and writes the two slots the scene hands out,
        /// `mDiffuse` and `mMaskOffset`. Starts as `wholeLayer`.
        MaterialLayer mRow = wholeLayer();
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

        /// Empty for every part no normal map is read through — `MeshArrays::mTangents`.
        Run mTangents;

        /// Empty for every part but the few that bind a second set — `MeshArrays::mSecondTexCoords`.
        Run mSecondTexCoords;
        std::uint32_t mUnitStreams = 0;

        Run mIndices;

        FoldedShape mShape;
    };

    /// A model read whole on a thread that is not the frame's: its parts, the folded geometry of
    /// every one of them in the format the scene copies from, and the images they name. Owned by
    /// the reader and lent to every cell that names it, once per cell — `Lent` counts them;
    /// refilled for the next model once every hold is back. Flat, one buffer per attribute,
    /// because a cell names hundreds of models of a handful of parts each.
    struct PreparedModel : Lent
    {
        /// The corrected path the model was read under, which is what the reader finds it by.
        std::string mPath;

        /// Held, because every part's drawable and state set are the template's own.
        osg::ref_ptr<const osg::Node> mTemplate;

        /// The template's own bound, which is what the paging measured a reference by.
        float mRadius = 0.0f;

        std::vector<PreparedPart> mParts;

        std::vector<osg::Vec3f> mPositions;
        std::vector<osg::Vec3f> mNormals;
        std::vector<osg::Vec2f> mTexCoords;
        std::vector<osg::Vec2f> mSecondTexCoords;
        std::vector<osg::Vec3f> mColours;
        std::vector<osg::Vec4f> mTangents;
        std::vector<std::uint32_t> mIndices;

        /// Why its walk refused the template, or empty. A refused model holds no part and is filed
        /// under its path like any other, so the next reference to it is not walked again.
        std::string mRefused;

        /// What one of its parts comes to, as the frame adopts it. The spans are into this model's
        /// own storage, and live for as long as it is lent.
        MeshReading readingOf(const PreparedPart& part) const
        {
            return MeshReading{
                .mArrays = {
                    .mPositions = part.mVertices.in(std::span<const osg::Vec3f>(mPositions)),
                    .mNormals = part.mNormals.in(std::span<const osg::Vec3f>(mNormals)),
                    .mTexCoords = part.mTexCoords.in(std::span<const osg::Vec2f>(mTexCoords)),
                    .mSecondTexCoords = part.mSecondTexCoords.in(std::span<const osg::Vec2f>(mSecondTexCoords)),
                    .mUnitStreams = part.mUnitStreams,
                    .mColours = part.mColours.in(std::span<const osg::Vec3f>(mColours)),
                    .mTangents = part.mTangents.in(std::span<const osg::Vec4f>(mTangents)),
                    .mIndices = part.mIndices.in(std::span<const std::uint32_t>(mIndices)),
                },
                .mShape = part.mShape,
            };
        }

        /// Makes room for the next model, keeping what the buffers grew.
        void reuse()
        {
            reuseKeeping(*this, &PreparedModel::mPath, &PreparedModel::mParts, &PreparedModel::mPositions,
                &PreparedModel::mNormals, &PreparedModel::mTexCoords, &PreparedModel::mSecondTexCoords,
                &PreparedModel::mColours, &PreparedModel::mTangents, &PreparedModel::mIndices,
                &PreparedModel::mRefused);
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

    /// One `LIGH` reference a cell stands: where it stands and what its record says. The record
    /// and not a light, because a light is a function of the hour — a flame flickers — and the
    /// frame builds one from this every walk (`Rtx::makeLight`).
    struct PreparedLight
    {
        /// The reference's own origin, and not the model's `AttachLight` node. Finding that means
        /// loading the mesh, and the mesh is what the paging refuses to stand out here; the offset
        /// between the two is the height of a lamp, against a cell of distance.
        osg::Vec3f mPosition;

        ESM::RefNum mRefNum;
        SceneUtil::LightCommon mRecord;
    };

    /// What a thread hands the frame for one cell: its ground, the references that page, reduced
    /// the way the content files stack, the models they name, and the lights the paging never
    /// draws. Owned by the reader and lent to the frame, as a model is.
    struct PreparedCell : Lent
    {
        osg::Vec2i mCell;

        /// Whether the references were read at all, which is the ring's statics switch as it stood
        /// when the cell was read.
        bool mStatics = false;

        PreparedGround mGround;

        /// Every model the references name, once each.
        std::vector<PreparedModel*> mModels;

        std::vector<PreparedRef> mRefs;

        /// The `LIGH` references whose record casts where it is placed, whether or not the statics
        /// were read — `CellRing` says why the ring stands lamps at all.
        std::vector<PreparedLight> mLights;

        /// The models and the lamps of the cell this renderer cannot take, which only the frame's
        /// thread reports.
        std::vector<Refusal> mRefusals;

        void reuse()
        {
            reuseKeeping(*this, &PreparedCell::mGround, &PreparedCell::mModels, &PreparedCell::mRefs,
                &PreparedCell::mLights, &PreparedCell::mRefusals);
        }
    };
}

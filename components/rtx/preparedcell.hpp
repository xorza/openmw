#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <osg/Drawable>
#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Vec2f>
#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/esm3/refnum.hpp>

#include "materialresolver.hpp"
#include "preparedground.hpp"
#include "shapefold.hpp"

namespace Rtx
{
    struct PreparedTexture;

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

        std::uint32_t mFirstVertex = 0;
        std::uint32_t mVertexCount = 0;

        /// Nought where the geometry names no normal, and no texture coordinate.
        std::uint32_t mFirstNormal = 0;
        std::uint32_t mNormalCount = 0;
        std::uint32_t mFirstTexCoord = 0;
        std::uint32_t mTexCoordCount = 0;

        std::uint32_t mFirstIndex = 0;
        std::uint32_t mIndexCount = 0;

        FoldedShape mShape;
    };

    /// A model read whole on a thread that is not the frame's: its parts, the folded geometry of
    /// every one of them in the format the scene copies from, and the images they name.
    ///
    /// **Owned by the reader that made it and lent to every cell that names it, once per cell.**
    /// The frame holds it by address for as long as some cell it adopted or is about to adopt
    /// names it, and gives each cell's hold back through the ring when it lets that cell go; the
    /// reader refills the model for the next one once every hold is back. `Spares` says why an
    /// address and not a shared count.
    ///
    /// **Flat, and one buffer per attribute.** A model is a handful of parts and a cell names
    /// hundreds of models, so a vector per part per attribute is an allocation per part on the
    /// thread that reads it — and buffers that are kept refill rather than grow.
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
        std::uint32_t mHolders = 0;

        std::vector<PreparedPart> mParts;

        /// Every image the parts name, once each. The reader's, lent for as long as this is.
        std::vector<PreparedTexture*> mTextures;

        std::vector<osg::Vec3f> mPositions;
        std::vector<osg::Vec3f> mNormals;
        std::vector<osg::Vec2f> mTexCoords;
        std::vector<std::uint32_t> mIndices;

        /// Makes room for the next model, keeping what the buffers grew.
        void reuse()
        {
            mPath.clear();
            mTemplate = nullptr;
            mRadius = 0.0f;
            mHolders = 0;
            mParts.clear();
            mTextures.clear();
            mPositions.clear();
            mNormals.clear();
            mTexCoords.clear();
            mIndices.clear();
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

        void reuse()
        {
            mStatics = false;
            mGround.reuse();
            mModels.clear();
            mRefs.clear();
        }
    };
}

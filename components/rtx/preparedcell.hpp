#pragma once

#include <cstdint>
#include <span>
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
#include "meshreader.hpp"
#include "preparedground.hpp"
#include "run.hpp"
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

        /// Where this part's attributes sit in the model's own buffers.
        ///
        /// **Runs, and not five pairs of an offset and a count.** `Rtx::Run` says why the two halves
        /// travel together: a reader that paired one run's offset with another's count would index a
        /// buffer that exists, by a length that is not its own.
        Run mVertices;

        /// Empty where the geometry names no normal, no texture coordinate and no colour.
        Run mNormals;
        Run mTexCoords;
        Run mColours;

        Run mIndices;

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
        std::uint32_t mLent = 0;

        std::vector<PreparedPart> mParts;

        /// Every image the parts name, once each. The reader's, lent for as long as this is.
        std::vector<PreparedTexture*> mTextures;

        std::vector<osg::Vec3f> mPositions;
        std::vector<osg::Vec3f> mNormals;
        std::vector<osg::Vec2f> mTexCoords;
        std::vector<osg::Vec3f> mColours;
        std::vector<std::uint32_t> mIndices;

        /// What one of its parts comes to, as the frame adopts it.
        ///
        /// **Here rather than beside the one caller, because the buffers are this model's.** A part
        /// holds five runs and this holds what they name, so the pairing is one statement in one
        /// place — and `MeshReading` is exactly what a reader made and what the frame takes.
        ///
        /// The spans are into this model's own storage, and live for as long as it is lent.
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
            mPath.clear();
            mTemplate = nullptr;
            mRadius = 0.0f;
            mLent = 0;
            mParts.clear();
            mTextures.clear();
            mPositions.clear();
            mNormals.clear();
            mTexCoords.clear();
            mColours.clear();
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

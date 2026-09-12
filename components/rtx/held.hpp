#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <osg/Matrixf>
#include <osg/Vec2i>
#include <osg/Vec3f>

#include <components/esm3/refnum.hpp>

#include "prepared.hpp"
#include "residency.hpp"
#include "runs.hpp"
#include "scratch.hpp"
#include "slots.hpp"

namespace osg
{
    class Drawable;
    class Image;
    class StateSet;
}

namespace Rtx
{
    /// One placement the ring may stand: a part of a model at a reference.
    struct Placement
    {
        Index mMesh = sNoIndex;
        Index mMaterial = sNoIndex;
        osg::Matrixf mTransform;

        /// What the size rule reads. A cell's placements are sorted by it, largest first, so what
        /// the rule admits at any threshold is a prefix — `HeldCell::mShown`.
        float mRadius = 0.0f;
        ESM::RefNum mRefNum;

        /// A script has disabled the reference: no slot however large it is. Set as the cell is
        /// adopted and flipped by `CellPlacer::setReferenceEnabled`, which is what keeps it off
        /// the walk every frame makes.
        bool mDisabled = false;

        /// The slot it stands in, or none while the size rule, a script or the ring keeps it out.
        Index mSlot = sNoIndex;
    };

    /// A cell's ground as the frame holds it: its rows, where it stands, and what shades it. Its
    /// own type, because the seven are empty together.
    struct HeldGround
    {
        /// The rows the ring holds on the scene, which no drawable and no state set will ever name.
        Index mMesh = sNoIndex;
        Index mMaterial = sNoIndex;

        /// The slot it stands in, or none while the ring keeps it out.
        Index mSlot = sNoIndex;

        /// The cell's centre, which the mesh's own positions are relative to.
        osg::Vec3f mOrigin;

        /// How many layers the stack holds, which is whether a composite is worth asking for at all.
        std::uint32_t mLayers = 0;

        /// Whether the material row asks for a composite now, so a crossing of the active grid
        /// rewrites it once.
        bool mFlattened = false;

        /// The readings of its textures, held for as long as the cell is.
        std::vector<PreparedTexture*> mTextures;

        /// Empties it for the next cell, keeping the room the texture list grew.
        void reuse() { reuseKeeping(*this, &HeldGround::mTextures); }
    };

    /// A cell the frame has adopted. Its vectors are kept when it is dropped, so a cell that arrives
    /// later refills them rather than growing new ones — `Recycled`.
    struct HeldCell
    {
        osg::Vec2i mCell;

        /// Whether its references were read, so a cell read under the other setting is dropped and
        /// read again.
        bool mStatics = false;

        /// Marked by the ring's sweep and compacted after it, so a frame that drops many cells
        /// shifts the table once.
        bool mDropped = false;

        /// Largest radius first, which `CellPlacer::adoptPlacements` sorts once.
        std::vector<Placement> mPlacements;

        /// How many of `mPlacements` the size rule admitted on the last `CellPlacer::place`: every
        /// placement before this stands unless a script disabled it, and none after it does. What a
        /// walk touches is the two ends of this and not the vector.
        std::size_t mShown = 0;

        std::vector<PreparedModel*> mModels;

        /// The ground, or nothing where the land names none. What is emptied on a drop is the
        /// optional's contents and never the optional itself, so its texture list keeps its room.
        std::optional<HeldGround> mGround;
    };

    /// What the frame holds of the models and the images the reader lent it, and what it adopted
    /// them as — the bookkeeping half of the cell ring. The count here is the frame's, and the
    /// reader keeps one of its own, because it lends to cells the frame has not seen yet.
    class CellHolds
    {
    public:
        /// One part of a model as the scene holds it, and what it was adopted under — kept here
        /// rather than read off the model at release, because a world that is detached lets go of
        /// its models before the walk that could release them runs. Raw, because the identity maps
        /// own both for as long as the holds stand.
        struct AdoptedPart
        {
            Index mMesh = sNoIndex;
            Index mMaterial = sNoIndex;

            const osg::Drawable* mDrawable = nullptr;
            const osg::StateSet* mKey = nullptr;
        };

        /// A model the frame knows of: named by a cell it holds or by one the reader has handed
        /// over and it has not adopted. Both counts, because the frame forgets a model only when
        /// neither names it.
        struct HeldModel
        {
            PreparedModel* mModel = nullptr;

            /// Empty until the first cell naming the model is adopted.
            std::vector<AdoptedPart> mParts;

            /// Cells adopted that name it, and cells handed over and not yet adopted that do.
            std::uint32_t mHeld = 0;
            std::uint32_t mHanded = 0;
        };

        /// The entry for `model`, made where the frame knows of none — and its images held for
        /// `find` when it is.
        HeldModel& know(PreparedModel& model);

        /// The entry for a model the frame knows of.
        HeldModel& knownOf(const PreparedModel& model);

        /// Adopts a model's parts into the scene, where a cell first stands it. One hold per part on
        /// the mesh and on the material, which `release` gives back.
        void adoptParts(HeldModel& held, SceneAdopter& into);

        /// Counts one cell of `model` off, and forgets the model where none is left: its images
        /// stop being found, and its parts' holds go back on the next `releaseParts`.
        void release(PreparedModel& model, bool wasHeld);

        /// Counts one more holder of `texture`'s reading, for `find`.
        void holdTexture(const PreparedTexture& texture);

        /// Counts one holder off, and forgets the reading where none is left.
        void dropTexture(const PreparedTexture& texture);

        /// The reading of `image` where a model or a cell's ground the frame holds names it.
        const PreparedTexture* find(const osg::Image& image) const;

        /// Gives the holds of every part `release` and `forget` let go of back to the walk,
        /// deferred because a release can happen outside one and only the walk can reach the
        /// resolvers. Run at both ends of a walk.
        void releaseParts(SceneAdopter& into);

        /// Lets go of every model and every image, keeping their parts' holds for `releaseParts`.
        /// What the reader lent dies with it, so nothing is given back here.
        void forget();

    private:
        /// An image some known model or held ground names, and its reading, for `find`.
        struct HeldTexture
        {
            const osg::Image* mImage = nullptr;
            const PreparedTexture* mTexture = nullptr;
            std::uint32_t mHolders = 0;
        };

        /// What each of the two tables is ordered by, stated once each so that no search can
        /// disagree with the insertion it is looking for.
        struct ModelAt
        {
            const PreparedModel* operator()(const HeldModel& held) const { return held.mModel; }
        };

        struct TextureAt
        {
            const osg::Image* operator()(const HeldTexture& held) const { return held.mImage; }
        };

        SortedRows<HeldModel, const PreparedModel*, ModelAt> mModels;

        /// A model's row taken back out is room the next one refills rather than a heap call on
        /// the frame a cell lands.
        Recycled<HeldModel> mSpareModels;

        SortedRows<HeldTexture, const osg::Image*, TextureAt> mTextures;

        /// Parts let go of and not yet released to the walk. See `releaseParts`.
        std::vector<AdoptedPart> mReleasing;
    };
}

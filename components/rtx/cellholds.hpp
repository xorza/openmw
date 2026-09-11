#pragma once

#include <cstdint>
#include <vector>

#include "index.hpp"
#include "preparedcell.hpp"
#include "preparedtexture.hpp"
#include "recycled.hpp"
#include "residency.hpp"
#include "sortedrows.hpp"

namespace osg
{
    class Drawable;
    class Image;
    class StateSet;
}

namespace Rtx
{
    /// What the frame holds of the models and the images the reader lent it, and what it adopted
    /// them as.
    ///
    /// **The bookkeeping half of the cell ring, apart from the policy.** Which cells are wanted and
    /// where they stand are `CellRing`'s and `CellPlacer`'s; this is the count of cells naming each
    /// model, the parts each model was adopted as, the images the readings are found for, and the
    /// holds on the scene those adoptions took.
    ///
    /// **The count here is the frame's, and the reader keeps one of its own.** The reader lends a
    /// model once to every cell that names it and counts what it lent, because it lends to cells the
    /// frame has not seen yet; this counts the cells the frame holds or is about to adopt, because
    /// that is when the parts are adopted and when they are let go of. Two facts, two counts.
    class CellHolds
    {
    public:
        /// One part of a model as the scene holds it, and what it was adopted under.
        ///
        /// **The drawable and the state set are what the holds are given back by**, and they are
        /// kept here rather than read off the model at release, because a world that is detached
        /// lets go of its models before the walk that could release them runs — `releaseParts`.
        /// Raw, because the identity maps own both for as long as the holds stand.
        struct AdoptedPart
        {
            Index mMesh = sNoIndex;
            Index mMaterial = sNoIndex;

            const osg::Drawable* mDrawable = nullptr;
            const osg::StateSet* mKey = nullptr;
        };

        /// A model the frame knows of: named by a cell it holds or by one the reader has handed
        /// over and it has not adopted.
        ///
        /// **Both counts, because the frame forgets a model only when neither names it** — its
        /// parts are adopted once, and its images are found for as long as it is known.
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

        /// Gives the holds of every part `release` and `forget` let go of back to the walk.
        ///
        /// **Deferred to the walk rather than given back on the spot**, because a release can happen
        /// outside one — `forget` runs from `follow`, before the walk — and only the walk can reach
        /// the resolvers. Run at both ends of a walk, so a part let go of inside a walk goes in that
        /// walk and one let go of between walks goes in the next.
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

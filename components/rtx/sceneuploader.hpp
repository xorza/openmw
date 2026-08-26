#pragma once

#include <cstddef>
#include <cstdint>

#include "compositequeue.hpp"
#include "renderer.hpp"
#include "wavespectrum.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class SceneDesc;

    /// What handing a mirrored scene to a renderer came to.
    struct SceneUpload
    {
        enum class Kind
        {
            /// Nothing arrived: the slots that moved had their transforms rewritten and that is all.
            Placed,
            /// Geometry arrived and was appended — new structures built, new textures added to the
            /// array, everything already there left where it was.
            Extended,
            /// The indices moved, so everything built from them was built again.
            Rebuilt,
        };

        Kind mKind = Kind::Placed;

        /// How many textures had to be described, which is zero on a `Placed`.
        std::size_t mDescribed = 0;

        /// How many of those could not be read and got the stand-in. See `SceneTextures`.
        std::uint32_t mUnreadable = 0;

        /// How many texture slots the scene gave back, whose images the renderer was told to drop.
        ///
        /// **Not zero on a `Placed`**, which is the point of counting it: leaving a region is a frame
        /// where nothing arrives, and waiting for the next arrival to give the memory back is what
        /// made the island route settle at what it had visited.
        std::size_t mDropped = 0;
    };

    /// Takes, once a frame, the cheapest of the three ways to hand a mirrored scene over.
    ///
    /// **The decision is the same one in the game and in the harness, so it is written once.** The
    /// three calls behind it are three orders of magnitude apart — a place is under a millisecond, an
    /// extend is a few, a rebuild is a fifth of a second because the texture array is made again from
    /// nothing — and choosing wrongly in either direction is fatal rather than slow: place a scene
    /// that gained a mesh and the frame names a bottom-level structure that does not exist; place one
    /// that was compacted and every index points at something else.
    ///
    /// One per renderer and scene, and it checks rather than trusting that: a pairing it does not
    /// recognise is built from nothing rather than appended to.
    class SceneUploader
    {
    public:
        /// Hands `scene` to `renderer`, building only what has to be built.
        ///
        /// `scene` must have been walked this frame — placements cleared and re-extracted — because
        /// what the three branches differ over is what to do *besides* placing.
        /// Takes the scene by mutable reference because it consumes its arrivals: what a walk added
        /// is uploaded here and forgotten here, so a caller cannot upload it twice or lose it.
        /// @param slot which of the renderer's scenes: `sWorld`, or one `addViewScene` gave
        ///        out. **A doll takes the same three branches a cell does** — a race-creation slider
        ///        drag redraws the same subject sixty times a second, and rebuilding it each time is
        ///        what this exists to stop.
        SceneUpload hand(Renderer& renderer, std::uint32_t slot, SceneDesc& scene, Resource::ImageManager& images,
            const SeaState& sea = SeaState{});

        /// Whether this serves a world staged once rather than a game that keeps running.
        ///
        /// **A caller with no next frame cannot leave a bake unfinished.** Flattening a chunk's
        /// ground costs 28.5 ms, so a running game drains a slice of it per frame and the chunk
        /// shades from its layer stack until one arrives — half a second of a cost per hit instead of
        /// a quarter-second hitch. A harness that stages a region, renders one frame and stops has
        /// nowhere to put the rest, and would photograph a picture no player ever sees. Told once,
        /// because it is a fact about the caller and not about the frame.
        void setStaged(bool staged) { mStaged = staged; }

    private:
        /// **Whether the pair in front of it is the pair it last built, and appending is only
        /// allowed onto that.**
        ///
        /// `bench` is what makes this a check instead of a rule in a comment: it runs several places
        /// through one renderer, a scene of its own for each, and a fresh uploader beside each scene.
        /// An uploader that took the renderer's texture count on faith would begin the second place's
        /// descriptions past the end of its own table — a hundreds-long overrun of a table that is
        /// hundreds long, and it read as a `std::bad_alloc` from somewhere else entirely.
        ///
        /// Compared and never dereferenced. The count is what carries the argument — an array
        /// somebody else left behind is not one to append to whatever its address was — and the two
        /// pointers are what stop a coincidence in it from mattering.
        bool recognises(
            const Renderer& renderer, std::uint32_t slot, const SceneDesc& scene, std::uint32_t textures) const;

        bool mStaged = false;

        /// The distant chunks waiting for their ground to be flattened.
        ///
        /// **Here because this is the once-a-frame call**, and because a bake spread over frames has
        /// to outlive the one that asked for it. `SceneTextures` is built and thrown away inside
        /// `hand`; what is waiting cannot be.
        CompositeQueue mComposites;

        const Renderer* mRenderer = nullptr;
        const SceneDesc* mScene = nullptr;

        /// Which of that renderer's scenes, so an uploader cannot append a doll onto the world.
        std::uint32_t mSlot = sWorld;

        /// How long the renderer's texture array was when this last left it.
        std::uint32_t mUploaded = 0;

        /// Which revision of the scene the structures and the texture array were built from.
        ///
        /// **A counter and not a set of table sizes**, because walking across a cell boundary loses
        /// one cell as it gains another: a scene that ends the frame the same size it started is
        /// exactly the case a size comparison misses, and the structures it kept describe geometry
        /// that has gone.
        std::uint64_t mBuilt = 0;

        /// The reset those were built against, so a scene replaced outright is told from one that
        /// grew.
        std::uint64_t mReset = 0;
    };
}

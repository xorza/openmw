#pragma once

#include <cstddef>
#include <cstdint>

#include "framespend.hpp"
#include "renderer.hpp"
#include "texturebuilder.hpp"
#include "wavespectrum.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class CompositeQueue;
    class SceneDesc;
    class SceneSink;

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
            /// This uploader had built nothing for this renderer and this scene, so there was
            /// nothing to append to and every array was made from the whole table.
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
    /// **Stateless against the backend.** Whether the backend holds this scene, and at which
    /// revision, is the backend's to say — `SceneSink::describeHeld` — so a scene it does not hold
    /// is built from nothing rather than appended to, whichever uploader asks.
    class SceneUploader
    {
    public:
        /// Hands `scene` to `renderer`, building only what has to be built.
        ///
        /// `scene` must have been walked this frame — placements cleared and re-extracted — because
        /// what the three branches differ over is what to do *besides* placing.
        /// Takes the scene by mutable reference because it consumes its arrivals: what a walk added
        /// is uploaded here and forgotten here, so a caller cannot upload it twice or lose it.
        /// @param slot which of the renderer's scenes: the world's, or one `addViewScene` gave
        ///        out. **A doll takes the same three branches a cell does** — a race-creation slider
        ///        drag redraws the same subject sixty times a second, and rebuilding it each time is
        ///        what this exists to stop.
        /// @param composites the world's terrain baker, or null for a scene with no distant ground.
        ///        **Not a member, because flattening a chunk is the world's business and not the
        ///        hand-over's**: a doll and a map tile go through the same three branches and
        ///        neither has ground to flatten, so an uploader of its own would carry a mutex, a
        ///        thread and a shading cache to bake nothing.
        /// @param readings where a describe finds images read ahead of the frame, or null.
        /// @param spend where the three halves of the hand-over are timed into, or null. `Bake` is
        ///        the ground the composite queue handed back, `Textures` the arrived textures being
        ///        opened and described, `Upload` the renderer being told — whichever of the three
        ///        calls this was, and the texture slots given back beside it. **Timed here and not
        ///        inside the backend**, because what the three have in common is that they are the
        ///        hand-over, and a backend that timed itself would be answering a question about the
        ///        host's frame.
        SceneUpload hand(SceneSink& renderer, SceneSlot slot, SceneDesc& scene, Resource::ImageManager& images,
            CompositeQueue* composites, const SeaState& sea = SeaState{}, const TextureReadings* readings = nullptr,
            FrameSpend* spend = nullptr);

    private:
        /// What an arrival is described into, and the storage the descriptions point at.
        ///
        /// **Held, so an arrival frame does not pay for the buffers.** Every vector inside settles
        /// at the busiest cell the run has met, and each arrival clears and refills them. Nothing
        /// reads them between calls: `setScene` and `extendScene` are done with the spans when they
        /// return.
        SceneTextures mTextures;
    };
}

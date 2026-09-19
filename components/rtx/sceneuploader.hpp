#pragma once

#include <cstddef>
#include <cstdint>

#include "framespend.hpp"
#include "renderer.hpp"
#include "slot.hpp"
#include "texturebuilder.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class CompositeQueue;
    class SceneDesc;
    class Renderer;

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

        /// How many meshes the scene gained since the last hand-over, which an `Extended` or a
        /// `Rebuilt` upload built structures for on the frame's own queue. Nought for a `Placed`.
        std::uint32_t mArrivedMeshes = 0;

        /// How many textures had to be described, which is zero on a `Placed`.
        std::size_t mDescribed = 0;

        /// How many of those could not be read and got the stand-in. See `SceneTextures`.
        std::uint32_t mUnreadable = 0;

        /// How many texture slots the scene gave back, whose images the renderer was told to drop.
        /// Not zero on a `Placed`: leaving a region is a frame where nothing arrives.
        std::size_t mDropped = 0;
    };

    /// Takes, once a frame, the cheapest of the three ways to hand a mirrored scene over, written
    /// once for the game and the harness: a place is under a millisecond, an extend a few, a
    /// rebuild a fifth of a second, and choosing wrongly is fatal rather than slow. Stateless
    /// against the backend, which says what it holds and which description it holds it from
    /// through `Renderer::describeHeld`.
    class SceneUploader
    {
    public:
        /// What one hand-over is of. The three pointers are what a caller may not have.
        struct Handing
        {
            /// Which of the renderer's scenes. A doll takes the same three branches a cell does: a
            /// slider drag redraws the same subject sixty times a second.
            SceneSlot mSlot;

            /// Walked this frame, and mutable because its arrivals are consumed here, so a caller
            /// cannot upload them twice or lose them.
            SceneDesc& mScene;

            Resource::ImageManager& mImages;

            /// The world's terrain baker, or null for a scene with no distant ground: a doll and a
            /// map tile have none to flatten.
            CompositeQueue* mComposites = nullptr;

            /// Where the three halves of the hand-over are timed into, or null — `Bake`, `Textures`
            /// and `Upload`. Timed here, because a backend that timed itself would be answering a
            /// question about the host's frame.
            FrameSpend* mSpend = nullptr;

            /// Whether the hand-over ends the placement it handed over — `PlacementTable::advance`,
            /// so what moved becomes where things were. Here and nowhere else, because the
            /// hand-over is the one thing that reads the change lists through a backend, and a
            /// slot that left the lists unread would stand on the device as it stood before. The
            /// world's frame says yes. A picture of a subject says no: it is drawn when the
            /// subject changes and not when the frame does, has no motion to describe, and a
            /// scene that never advanced answers a previous transform equal to the current one,
            /// which is the right answer there and a stale one otherwise.
            bool mAdvance = true;
        };

        /// Hands the scene to `renderer`, building only what has to be built.
        SceneUpload hand(Renderer& renderer, const Handing& handing);

    private:
        /// What an arrival is described into, and the storage the descriptions point at. Held, so
        /// an arrival frame does not pay for the buffers; nothing reads them between calls.
        SceneTextures mTextures;
    };
}

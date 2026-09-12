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
    /// against the backend, which says what it holds through `Renderer::describeHeld`.
    class SceneUploader
    {
    public:
        /// Hands `scene` to `renderer`, building only what has to be built. `scene` must have been
        /// walked this frame, and is taken by mutable reference because its arrivals are consumed
        /// here, so a caller cannot upload them twice or lose them.
        /// @param slot which of the renderer's scenes. A doll takes the same three branches a cell
        ///        does: a slider drag redraws the same subject sixty times a second.
        /// @param composites the world's terrain baker, or null for a scene with no distant ground.
        ///        Not a member, because a doll and a map tile have no ground to flatten.
        /// @param readings where a describe finds images read ahead of the frame, or null.
        /// @param spend where the three halves of the hand-over are timed into, or null — `Bake`,
        ///        `Textures` and `Upload`. Timed here, because a backend that timed itself would be
        ///        answering a question about the host's frame.
        /// What one hand-over is of. The three pointers are what a caller may not have.
        struct Handing
        {
            SceneSlot mSlot;
            SceneDesc& mScene;
            Resource::ImageManager& mImages;
            CompositeQueue* mComposites = nullptr;
            SeaState mSea{};
            const TextureReadings* mReadings = nullptr;
            FrameSpend* mSpend = nullptr;
        };

        SceneUpload hand(Renderer& renderer, const Handing& handing);

    private:
        /// Whether this uploader has ever built its scene into a backend. With the backend's own
        /// answer, what decides between building and appending.
        bool mBuilt = false;

        /// What an arrival is described into, and the storage the descriptions point at. Held, so
        /// an arrival frame does not pay for the buffers; nothing reads them between calls.
        SceneTextures mTextures;
    };
}

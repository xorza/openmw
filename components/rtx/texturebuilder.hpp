#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Image>

#include "alphaimage.hpp"
#include "index.hpp"
#include "mipchain.hpp"
#include "pool.hpp"
#include "spritelight.hpp"
#include "texturedata.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class CompositeQueue;
    struct SceneTables;

    /// Describes one image for a backend's uploader without copying a byte of it.
    ///
    /// Levels are **appended** to `levels`, and the returned description spans the ones it added —
    /// so `levels` must outlive the upload and must not grow again while the description is alive.
    /// `SceneTextures` is what reserves for that.
    ///
    /// The levels are the file's own, however few it carried. `MipChain` is what builds the rest,
    /// and `SceneTextures` is what asks it to.
    ///
    /// Throws for a format Morrowind does not produce: inventing a conversion path for something no
    /// content file contains is how a renderer grows code nothing runs.
    TextureData describeImage(const osg::Image& image, std::vector<MipLevel>& levels);

    /// The image at `path`, or null where nothing could be read there.
    ///
    /// **Null and not an exception**, because a live scene graph names textures that were never
    /// files and a renderer that fell over on one would fall over on a cell.
    osg::ref_ptr<const osg::Image> openImage(Resource::ImageManager& images, const VFS::Path::Normalized& path);

    /// Every live texture a scene names, described, and the storage those descriptions point into.
    ///
    /// **Each description carries the slot it belongs to and there is not one per slot.** A slot the
    /// scene has given back is passed over rather than described, so a backend writes what arrived
    /// and leaves the rest of its array alone.
    ///
    /// **This is where the core stops and a backend starts.** `TextureData` carries spans rather
    /// than bytes, so something has to own the decoded images and the level table while a backend
    /// reads them; this is that, and it knows no graphics API. Which of them becomes a `VkImage` or
    /// an `MTLTexture` is the backend's business and none of this one's.
    ///
    /// Non-copyable and non-movable because the descriptions point into its own vectors, and because
    /// it is a member of whatever hands scenes over rather than a value passed about.
    ///
    /// **Held for the life of its owner, and cleared and refilled per arrival.** Every buffer here
    /// settles at the busiest cell so far. Built and thrown away instead, it would grow every one
    /// of them on the frame a cell arrives — which is the frame with the least room for that.
    class SceneTextures
    {
    public:
        SceneTextures() = default;

        SceneTextures(const SceneTextures&) = delete;
        SceneTextures& operator=(const SceneTextures&) = delete;
        SceneTextures(SceneTextures&&) = delete;
        SceneTextures& operator=(SceneTextures&&) = delete;

        /// Resolves and describes every texture `scene` still names, in table order.
        ///
        /// For a backend building an array from nothing. The free slots are not among them, so the
        /// array has to be sized to the scene's table rather than to what comes out of here.
        /// @param composites where a chunk's flattened ground comes from, or null for a caller
        ///        that bakes none. A terrain slot the queue has no composite for is one whose bake
        ///        has not finished, and it is passed over rather than described — nothing points at
        ///        it until it has bytes.
        void describeAll(
            const SceneTables& scene, Resource::ImageManager& images, const CompositeQueue* composites = nullptr);

        /// The same, for `slots` and nothing else.
        ///
        /// **This is what stops a texture being decoded twice.** Describing reads the image and
        /// estimating its shading reads every texel of it, and a renderer that already holds three
        /// hundred needs neither done again for them — that repeated work is the 5% of the game's
        /// CPU that showed up as `ShadingMap` and `ColourBlock::read`.
        ///
        /// **A list and not an offset**, because a slot a departing cell freed is taken over
        /// wherever it sits: what arrived is no longer the end of the table. Each description
        /// carries the slot it belongs to, and a slot that has since been given back is skipped.
        void describe(const SceneTables& scene, Resource::ImageManager& images, std::span<const Index> slots,
            const CompositeQueue* composites = nullptr);

        /// What the last `describe` found, each carrying the slot it goes to in `TextureData::mSlot`.
        std::span<const TextureData> getDescriptions() const { return mDescriptions; }

        /// How many named a file that could not be read, each logged with its path where it was
        /// described.
        ///
        /// **Not zero in the game.** The harness names textures out of content files and every one
        /// of them is a `.dds` on disk; a live scene graph also holds textures that were never files
        /// — a terrain composite map rendered on the GPU, a render-to-texture target, something a
        /// script made. Those have no bytes to upload and no business bringing the renderer down.
        std::uint32_t getUnreadable() const { return mUnreadable; }

    private:
        /// One slot `describe` decided to describe, and what resolving it found.
        ///
        /// **One row and not three arrays sharing an index.** A slot, its image and which bake it
        /// is are decided together in one pass and read together in the next, and three vectors
        /// pushed in step are a rule a reader has to keep rather than a shape that keeps it.
        struct Kept
        {
            Index mSlot = sNoIndex;

            /// Which of `mSpriteLights` this slot's bake is, or `sNoIndex` where it is no such
            /// bake.
            Index mLight = sNoIndex;

            /// The file's image, or null where the slot names no file or nothing could be read
            /// there.
            osg::ref_ptr<const osg::Image> mImage;
        };

        // Refilled by every `describe` and never freed, so each settles at the busiest arrival so
        // far — which is where the room to grow one is least.

        /// The slots `describe` kept, because a free one is passed over and the descriptions are
        /// no longer one per entry of what it was asked for.
        std::vector<Kept> mKept;

        /// Every texture's estimated lighting, back to back and `SHADING_EXTENT` squared apiece.
        ///
        /// **Made on load and thrown away with the cell**, because a cache would cost more than it
        /// saved: a cell's couple of hundred textures estimate in well under a millisecond.
        std::vector<float> mShading;

        /// Every image's levels, back to back. One table rather than one vector each: a cell reaches
        /// a couple of hundred textures, and the descriptions want a span into something stable.
        std::vector<MipLevel> mLevels;

        std::vector<TextureData> mDescriptions;

        /// The bakes of the sprite textures the scene's emitters draw with, each made here from the
        /// alpha of the file its key names. `SpriteLightMap` says what one is.
        Pool<SpriteLightMap> mSpriteLights;

        /// The levels the files did not carry, for the few textures that carry none.
        ///
        /// **A pool of the chains that were built, and not one entry a texture.** Five thousand of
        /// Morrowind's textures carry a chain and a hundred and eighty-seven do not, so an entry a
        /// texture would be a pool the size of the cell — and each entry keeping its room means
        /// every position that ever held a 512-square chain keeps 1.4 MB for ever. Pooled instead,
        /// it is as deep as the most chains one arrival built, which is a handful.
        Pool<MipChain> mChains;

        /// Every slot of the scene's table, which is what a rebuild asks about. Held rather than
        /// built, because a rebuild is a fifth of a second and none of it should be this.
        std::vector<Index> mEverything;

        /// One sprite source while its bake is read, and nothing after: a source is described only
        /// to reach its alpha, and no description of it outlives the call. Held for the reason
        /// everything above is — a crossing bakes every emitter's sheet in one arrival.
        std::vector<MipLevel> mSourceLevels;
        MipChain mSourceChain;
        AlphaImage mSourceAlpha;

        std::uint32_t mUnreadable = 0;
    };
}

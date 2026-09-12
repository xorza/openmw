#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <osg/Image>

#include "alphaimage.hpp"
#include "mipchain.hpp"
#include "prepared.hpp"
#include "runs.hpp"
#include "scratch.hpp"
#include "spritelight.hpp"
#include "texturedata.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class CellHolds;
    class CompositeQueue;
    class SceneDesc;

    /// Describes one image for a backend's uploader without copying a byte of it. Levels are
    /// appended to `levels`, and the returned description spans the ones it added, so `levels`
    /// must not grow again while the description is alive. The levels are the file's own;
    /// `MipChain` builds the rest. Throws for a format Morrowind does not produce.
    TextureData describeImage(const osg::Image& image, std::vector<MipLevel>& levels);

    /// The image at `path`, or null where nothing could be read there — null and not an exception,
    /// because a live scene graph names textures that were never files and a renderer that fell
    /// over on one would fall over on a cell.
    osg::ref_ptr<const osg::Image> openImage(Resource::ImageManager& images, VFS::Path::NormalizedView path);

    /// Every live texture a scene names, described, and the storage those descriptions point into.
    /// Each description carries the slot it belongs to and there is not one per slot: a slot the
    /// scene has given back is passed over. `TextureData` carries spans rather than bytes, so this
    /// owns the decoded images and the level table while a backend reads them, and knows no
    /// graphics API. Non-copyable because the descriptions point into its own vectors; held for
    /// the life of its owner and refilled per arrival, so every buffer settles at the busiest cell.
    class SceneTextures
    {
    public:
        SceneTextures() = default;

        SceneTextures(const SceneTextures&) = delete;
        SceneTextures& operator=(const SceneTextures&) = delete;
        SceneTextures(SceneTextures&&) = delete;
        SceneTextures& operator=(SceneTextures&&) = delete;

        /// Resolves and describes every texture `scene` still names, in table order, for a backend
        /// building an array from nothing. The free slots are not among them.
        /// @param composites where a chunk's flattened ground comes from, or null for a caller
        ///        that bakes none. A terrain slot the queue has no composite for yet is passed over.
        /// @param readings the cell ring's holds, where images read ahead of the frame are found
        ///        by the image — a lookup instead of every texel for the chain and the shading
        ///        estimate — or null for a caller with none: a doll, a map tile, the harness's own
        ///        world.
        void describeAll(const SceneDesc& scene, Resource::ImageManager& images,
            const CompositeQueue* composites = nullptr, const CellHolds* readings = nullptr);

        /// The same, for `slots` and nothing else — what stops a texture being decoded and its
        /// shading estimated twice. A list and not an offset, because a slot a departing cell freed
        /// is taken over wherever it sits.
        void describe(const SceneDesc& scene, Resource::ImageManager& images, std::span<const Index> slots,
            const CompositeQueue* composites = nullptr, const CellHolds* readings = nullptr);

        /// What the last `describe` found, each carrying the slot it goes to in `TextureData::mSlot`.
        std::span<const TextureData> getDescriptions() const { return mDescriptions; }

        /// How many named a file that could not be read, each logged with its path. Not zero in the
        /// game: a live scene graph holds textures that were never files, and those have no
        /// business bringing the renderer down.
        std::uint32_t getUnreadable() const { return mUnreadable; }

    private:
        /// One slot `describe` decided to describe, and what resolving it found.
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
        /// Made on load and thrown away with the cell, because a cache would cost more than it
        /// saved: a cell's couple of hundred textures estimate in well under a millisecond.
        std::vector<float> mShading;

        /// Every image's levels, back to back. One table rather than one vector each: a cell reaches
        /// a couple of hundred textures, and the descriptions want a span into something stable.
        std::vector<MipLevel> mLevels;

        std::vector<TextureData> mDescriptions;

        /// The bakes of the sprite textures the scene's emitters draw with, each made here from the
        /// alpha of the file its key names. `SpriteLightMap` says what one is.
        Pool<SpriteLightMap> mSpriteLights;

        /// The levels the files did not carry, for the few textures that carry none — a hundred
        /// and eighty-seven of Morrowind's five thousand. A pool of the chains that were built and
        /// not one entry a texture, because an entry that ever held a 512-square chain keeps 1.4 MB.
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

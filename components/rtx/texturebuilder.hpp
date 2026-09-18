#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <osg/Image>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

#include "runs.hpp"
#include "texturedata.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class CompositeQueue;
    class SceneDesc;

    /// Describes one image for a backend's uploader without copying a byte of it. Levels are
    /// appended to `levels`, and the returned description spans the ones it added, so `levels`
    /// must not grow again while the description is alive. The levels are the file's own; a
    /// backend completes a chain the file did not carry, on the device. Throws for a format
    /// Morrowind does not produce.
    TextureData describeImage(const osg::Image& image, std::vector<MipLevel>& levels);

    /// The image at `path`, or null where nothing could be read there — null and not an exception,
    /// because a live scene graph names textures that were never files and a renderer that fell
    /// over on one would fall over on a cell.
    osg::ref_ptr<const osg::Image> openImage(Resource::ImageManager& images, VFS::Path::NormalizedView path);

    /// Every live texture a scene names, described, and the storage those descriptions point into.
    /// Each description carries the slot it belongs to and there is not one per slot: a slot the
    /// scene has given back is passed over. `TextureData` carries spans rather than bytes, so this
    /// holds the images and owns the level table while a backend reads them, and knows no
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
        /// @param composites which slots are chunks' flattened ground, or null for a caller that
        ///        flattens none. A terrain slot the queue did not give out is passed over.
        void describeAll(
            const SceneDesc& scene, Resource::ImageManager& images, const CompositeQueue* composites = nullptr);

        /// The same, for `slots` and nothing else — what stops a texture being described twice. A
        /// list and not an offset, because a slot a departing cell freed is taken over wherever it
        /// sits.
        void describe(const SceneDesc& scene, Resource::ImageManager& images, std::span<const Index> slots,
            const CompositeQueue* composites = nullptr);

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

            /// The slot of the sprite texture this slot's bake is made from on the device, or
            /// `sNoIndex` where the slot is no bake, or a bake whose source the table no longer
            /// holds.
            Index mBakedFrom = sNoIndex;

            /// The file's image, or null where the slot names no file or nothing could be read
            /// there.
            osg::ref_ptr<const osg::Image> mImage;
        };

        // Refilled by every `describe` and never freed, so each settles at the busiest arrival so
        // far — which is where the room to grow one is least.

        /// The slots `describe` kept, because a free one is passed over and the descriptions are
        /// no longer one per entry of what it was asked for.
        std::vector<Kept> mKept;

        /// Every image's levels, back to back. One table rather than one vector each: a cell reaches
        /// a couple of hundred textures, and the descriptions want a span into something stable.
        std::vector<MipLevel> mLevels;

        std::vector<TextureData> mDescriptions;

        /// Every slot of the scene's table, which is what a rebuild asks about. Held rather than
        /// built, because a rebuild is a fifth of a second and none of it should be this.
        std::vector<Index> mEverything;

        std::uint32_t mUnreadable = 0;
    };
}

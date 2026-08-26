#pragma once

#include <span>
#include <vector>

#include <osg/Image>

#include "scenedesc.hpp"
#include "texturedata.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class CompositeQueue;

    /// Describes one image for a backend's uploader without copying a byte of it.
    ///
    /// Levels are **appended** to `levels`, and the returned description spans the ones it added —
    /// so `levels` must outlive the upload and must not grow again while the description is alive.
    /// `SceneTextures` is what reserves for that.
    ///
    /// Throws for a format Morrowind does not produce: every texture in the game is block
    /// compressed with its mip chain already built, and inventing a conversion path for something no
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
    /// Non-copyable because the descriptions point into its own vectors. Moving is fine: a moved
    /// vector keeps the buffer they point at.
    class SceneTextures
    {
    public:
        /// Resolves and describes every texture `scene` still names, in table order.
        ///
        /// For a backend building an array from nothing. The free slots are not among them, so the
        /// array has to be sized to the scene's table rather than to what comes out of here.
        /// @param composites where a chunk's flattened ground comes from, or null for a caller
        ///        that bakes none. A terrain slot the queue has no composite for is one whose bake
        ///        has not finished, and it is passed over rather than described — nothing points at
        ///        it until it has bytes.
        SceneTextures(
            const SceneDesc& scene, Resource::ImageManager& images, const CompositeQueue* composites = nullptr);

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
        SceneTextures(const SceneDesc& scene, Resource::ImageManager& images, std::span<const Index> slots,
            const CompositeQueue* composites = nullptr);

        SceneTextures(const SceneTextures&) = delete;
        SceneTextures& operator=(const SceneTextures&) = delete;
        SceneTextures(SceneTextures&&) = default;
        SceneTextures& operator=(SceneTextures&&) = default;

        /// What was described, each carrying the slot it goes to in `TextureData::mSlot`.
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
        void describe(const SceneDesc& scene, Resource::ImageManager& images, std::span<const Index> slots,
            const CompositeQueue* composites);

        std::vector<osg::ref_ptr<const osg::Image>> mImages;

        /// Every texture's estimated lighting, back to back and `SHADING_EXTENT` squared apiece.
        ///
        /// **Made on load and thrown away with the cell**, because a cache would cost more than it
        /// saved: a cell's couple of hundred textures estimate in well under a millisecond.
        std::vector<float> mShading;

        /// Every image's levels, back to back. One table rather than one vector each: a cell reaches
        /// a couple of hundred textures, and the descriptions want a span into something stable.
        std::vector<MipLevel> mLevels;

        std::vector<TextureData> mDescriptions;

        std::uint32_t mUnreadable = 0;
    };
}

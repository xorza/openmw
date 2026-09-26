#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <osg/Image>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

#include "refusals.hpp"
#include "result.hpp"
#include "runs.hpp"
#include "texturedata.hpp"
#include "textureencoding.hpp"

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class CompositeQueue;
    class SceneDesc;

    /// What a texture that cannot stand is drawn as, described: one texel block of mid grey, in
    /// storage of its own that lasts the program. The one definition, which a backend stands once
    /// for every slot that draws it and a contact sheet draws as it is.
    TextureData describeStandIn();

    /// Whether this renderer uploads `image` as `encoding`, and why not where it does not: a format
    /// Morrowind does not produce, or an image of no size or no texels. The name is left to
    /// whoever reports it.
    Result<void, std::string> checkUploadable(
        const osg::Image& image, TextureEncoding encoding = TextureEncoding::Colour);

    /// Describes one image for a backend's uploader: the first slice of each level, spanned where
    /// the image holds them back to back in a format uploaded as it is, and laid into `texels`
    /// where it does not — widened from sixteen bits a texel, or gathered from a volume's levels.
    /// Levels are appended to `levels` and laid texels to `texels`, and the description spans what
    /// it added, so neither may grow again while it is alive. The levels are the file's own; a
    /// backend completes a chain the file did not carry, on the device. An error, adding nothing,
    /// where `checkUploadable` answers one, or where the format's layout and OpenSceneGraph's
    /// count the image's bytes differently.
    Result<TextureData, std::string> describeImage(const osg::Image& image, std::vector<MipLevel>& levels,
        std::vector<std::byte>& texels, TextureEncoding encoding = TextureEncoding::Colour);

    /// The image at `path`, or why nothing reads there — an error and not an exception, because a
    /// live scene graph names textures that were never files and a renderer that fell over on one
    /// would fall over on a cell. Never null.
    Result<osg::ref_ptr<const osg::Image>, std::string> openImage(
        Resource::ImageManager& images, VFS::Path::NormalizedView path);

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
        /// building an array from nothing. The free slots are not among them. A texture that cannot
        /// be described is described as the stand-in and refused, and so is the array running out
        /// of room.
        /// @param composites which slots are chunks' flattened ground, or null for a caller that
        ///        flattens none. A terrain slot the queue did not give out is described as the
        ///        stand-in and refused.
        void describeAll(
            const SceneDesc& scene, Resource::ImageManager& images, const CompositeQueue* composites = nullptr);

        /// The same, for `slots` and nothing else — what stops a texture being described twice. A
        /// list and not an offset, because a slot a departing cell freed is taken over wherever it
        /// sits.
        void describe(const SceneDesc& scene, Resource::ImageManager& images, std::span<const Index> slots,
            const CompositeQueue* composites = nullptr);

        /// What the last `describe` found, each carrying the slot it goes to in `TextureData::mSlot`.
        std::span<const TextureData> getDescriptions() const { return mDescriptions; }

        /// What the last `describe` refused, for whoever owns the scene to report: the scene is
        /// read here, because a read-only caller describes it too.
        std::span<const Refusal> getRefusals() const { return mRefusals; }

    private:
        /// One slot `describe` decided to describe, and what resolving it found.
        struct Kept
        {
            Index mSlot = sNoIndex;

            /// What the scene's table binds the slot as, which the file's bytes are read as.
            TextureEncoding mEncoding = TextureEncoding::Colour;

            /// For a bake, the slot of the sprite texture it is made from on the device, or
            /// `sNoIndex` where the table no longer holds that. Nothing for a slot that is no bake.
            std::optional<Index> mBakedFrom;

            /// The file's image, or why none reads. Null for a slot that names no file.
            Result<osg::ref_ptr<const osg::Image>, std::string> mImage = osg::ref_ptr<const osg::Image>();
        };

        /// What `kept` is described as, or why it gets the stand-in.
        Result<TextureData, std::string> describeKept(const Kept& kept, const CompositeQueue* composites);

        // Refilled by every `describe` and never freed, so each settles at the busiest arrival so
        // far — which is where the room to grow one is least.

        /// The slots `describe` kept, because a free one is passed over and the descriptions are
        /// no longer one per entry of what it was asked for.
        std::vector<Kept> mKept;

        /// Every image's levels, back to back. One table rather than one vector each: a cell reaches
        /// a couple of hundred textures, and the descriptions want a span into something stable.
        std::vector<MipLevel> mLevels;

        /// Every laid image's texels, back to back, for the same reason.
        std::vector<std::byte> mTexels;

        std::vector<TextureData> mDescriptions;

        /// Every slot of the scene's table, which is what a rebuild asks about. Held rather than
        /// built, because a rebuild is a fifth of a second and none of it should be this.
        std::vector<Index> mEverything;

        std::vector<Refusal> mRefusals;
    };
}

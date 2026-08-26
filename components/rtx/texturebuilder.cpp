#include "texturebuilder.hpp"

#include <algorithm>
#include <array>
#include <optional>

#include <osg/Image>

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>

#include "compositequeue.hpp"
#include "error.hpp"
#include "scenedesc.hpp"
#include "shadingmap.hpp"

namespace Rtx
{
    osg::ref_ptr<const osg::Image> openImage(Resource::ImageManager& images, const VFS::Path::Normalized& path)
    {
        try
        {
            return images.getImage(path);
        }
        catch (const std::exception&)
        {
            return nullptr;
        }
    }

    namespace
    {
        /// What OpenSceneGraph decoded, as one of the formats this renderer uploads.
        ///
        /// Nothing where the file is something else, so the caller says so with the file's name in
        /// the message. `TextureFormat` is why every case is sRGB.
        std::optional<TextureFormat> toTextureFormat(GLenum pixelFormat)
        {
            switch (pixelFormat)
            {
                // Both DXT1 spellings land on the format that reads the alpha bit, and the header
                // that claims there is none is not consulted. A BC1 block is punch-through whenever
                // its first endpoint sorts below its second, which is how every mask in the game is
                // stored; almost none of Morrowind's files set `DDPF_ALPHAPIXELS`, so believing the
                // header would decode that bit as opaque black and leave every canopy a solid card.
                // The bytes are identical either way — this only chooses whether the bit is looked at.
                case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
                case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
                    return TextureFormat::Bc1RgbaSrgb;
                case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
                    return TextureFormat::Bc2Srgb;
                case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                    return TextureFormat::Bc3Srgb;

                // **Not every file the game ships is a block.** The sky's cloud decks are plain
                // 32-bit `DDPF_RGB`, which is what a texture painted for a full-screen dome would
                // be, and taking only the compressed formats drew every weather's clouds grey.
                // Three-channel spellings are absent deliberately: a `GL_RGB` upload would need the
                // fourth channel written in, which means owning a buffer, and nothing in this game
                // stores an opaque texture without one.
                case GL_RGBA:
                    return TextureFormat::Rgba8Srgb;
                case GL_BGRA:
                    return TextureFormat::Bgra8Srgb;

                default:
                    return std::nullopt;
            }
        }

        /// What a texture that could not be read is drawn as.
        ///
        /// **Mid grey and not magenta.** A live graph's unreadable textures are mostly things that
        /// were never files — a composite map, a render target — and painting the ground magenta
        /// would be shouting about a case the count in `getUnreadable` already reports, at the price
        /// of the picture nobody can then judge.
        ///
        /// One BC1 block: both endpoints the same grey, so every one of its sixteen texels is too,
        /// and the first endpoint does not sort below the second, so it is opaque rather than
        /// punch-through.
        TextureData standIn(std::vector<MipLevel>& levels)
        {
            // 0x8410 is RGB565 for (16, 16, 16) out of (31, 63, 31) — a touch above half, which is
            // mid grey once the sRGB curve is undone.
            static constexpr std::array<std::byte, 8> sBlock{ std::byte{ 0x10 }, std::byte{ 0x84 }, std::byte{ 0x10 },
                std::byte{ 0x84 }, std::byte{}, std::byte{}, std::byte{}, std::byte{} };

            const std::size_t first = levels.size();
            levels.push_back(MipLevel{ 0, 4, 4 });

            return TextureData{
                .mFormat = TextureFormat::Bc1RgbaSrgb,
                .mWidth = 4,
                .mHeight = 4,
                .mBytes = sBlock,
                .mLevels = std::span<const MipLevel>(levels).subspan(first, 1),
                .mName = "unreadable",
            };
        }
    }

    TextureData describeImage(const osg::Image& image, std::vector<MipLevel>& levels)
    {
        const std::optional<TextureFormat> format = toTextureFormat(image.getPixelFormat());
        if (!format.has_value())
            throw Error("texture \"" + image.getFileName() + "\" is pixel format "
                + std::to_string(image.getPixelFormat()) + ", which is not one this renderer uploads");

        const auto width = static_cast<std::uint32_t>(image.s());
        const auto height = static_cast<std::uint32_t>(image.t());

        const std::size_t first = levels.size();
        const unsigned int count = image.getNumMipmapLevels();
        for (unsigned int level = 0; level < count; ++level)
            levels.push_back(MipLevel{
                .mOffset = image.getMipmapOffset(level),
                .mWidth = std::max(width >> level, 1u),
                .mHeight = std::max(height >> level, 1u),
            });

        return TextureData{
            .mFormat = *format,
            .mWidth = width,
            .mHeight = height,
            .mBytes
            = std::span(reinterpret_cast<const std::byte*>(image.data()), image.getTotalSizeInBytesIncludingMipmaps()),
            .mLevels = std::span<const MipLevel>(levels).subspan(first, count),
            .mName = image.getFileName(),
        };
    }

    SceneTextures::SceneTextures(
        const SceneDesc& scene, Resource::ImageManager& images, const CompositeQueue* composites)
    {
        std::vector<Index> everything(scene.getTextures().size());
        for (Index slot = 0; slot < everything.size(); ++slot)
            everything[slot] = slot;

        describe(scene, images, everything, composites);
    }

    SceneTextures::SceneTextures(const SceneDesc& scene, Resource::ImageManager& images, std::span<const Index> slots,
        const CompositeQueue* composites)
    {
        describe(scene, images, slots, composites);
    }

    void SceneTextures::describe(const SceneDesc& scene, Resource::ImageManager& images, std::span<const Index> slots,
        const CompositeQueue* composites)
    {
        // Which slots the loop below kept, because a free one is passed over and the descriptions
        // are no longer one per entry of `slots`.
        std::vector<Index> kept;
        kept.reserve(slots.size());
        mImages.reserve(slots.size());

        for (const Index slot : slots)
        {
            // **A free slot is not a texture.** `SceneDesc` empties one the last thing naming it
            // gave back and leaves it in the table until something takes it over; describing it
            // would build an image, a shading map and a descriptor write for a slot no material can
            // reach — and count it as a texture that arrived.
            if (scene.isTextureFree(slot))
                continue;

            // Already decoded and still resident: the scene manager keeps image data on the CPU
            // after apply, so this is a cache hit and a memcpy rather than a second decode.
            //
            // A slot this renderer made rather than opened has no file to be asked for. The entry
            // still has to exist, because the description below is built from it, and
            // A composite the queue has not finished is passed over the same way: nothing points
            // at a baked slot until it has bytes, so there is nothing here to describe yet.
            osg::ref_ptr<const osg::Image> image;
            if (scene.getBakedTextures()[slot].empty())
                image = openImage(images, scene.getTextures()[slot]);

            kept.push_back(slot);
            mImages.push_back(std::move(image));
        }

        // **Reserved exactly, and that is what makes the spans safe.** Every description points into
        // this one table, so it must not reallocate while they are being taken — and every level
        // count is known before the first description is built.
        std::size_t levels = 0;
        for (const osg::ref_ptr<const osg::Image>& image : mImages)
            levels += image != nullptr ? image->getNumMipmapLevels() : 1u;
        mLevels.reserve(levels);

        mDescriptions.reserve(mImages.size());
        for (std::size_t at = 0; at < mImages.size(); ++at)
        {
            const osg::ref_ptr<const osg::Image>& image = mImages[at];

            std::optional<TextureData> described;
            if (image != nullptr)
            {
                try
                {
                    described = describeImage(*image, mLevels);
                }
                catch (const Error&)
                {
                    described.reset();
                }
            }
            else if (const TerrainComposite* baked = composites != nullptr ? composites->find(kept[at]) : nullptr)
            {
                described = baked->describe();
            }

            if (!described.has_value())
            {
                ++mUnreadable;

                // Named rather than tallied, because a count says a texture is grey and nothing
                // about which one. On the frame a cell arrives, which is a load and not a frame
                // path.
                // **Whichever of the two named the slot**, or a composite that could not be
                // flattened reports itself as a file with no name — the one thing that would not
                // help in finding it.
                const std::string_view baked = scene.getBakedTextures()[kept[at]];
                Log(Debug::Warning) << "Texture \"" << (baked.empty() ? scene.getTextures()[kept[at]].value() : baked)
                                    << "\" could not be read; drawing the stand-in";

                described = standIn(mLevels);
            }

            described->mSlot = kept[at];
            mDescriptions.push_back(*described);
        }

        // After the descriptions, because the estimate reads the bytes they point at, and into one
        // table for the reason the levels are: the spans have to stay put.
        constexpr std::size_t cells = std::size_t{ ShadingMap::sExtent } * ShadingMap::sExtent;
        mShading.resize(mDescriptions.size() * cells);

        for (std::size_t i = 0; i < mDescriptions.size(); ++i)
        {
            const ShadingMap map(mDescriptions[i]);
            const std::span<const float> values = map.getValues();
            std::copy(values.begin(), values.end(), mShading.begin() + static_cast<std::ptrdiff_t>(i * cells));
        }

        for (std::size_t i = 0; i < mDescriptions.size(); ++i)
            mDescriptions[i].mShading = std::span(mShading).subspan(i * cells, cells);
    }
}

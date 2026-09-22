#include "texturebuilder.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <osg/Image>
#include <osg/ref_ptr>

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/vfs/pathutil.hpp>

#include "compositequeue.hpp"
#include "error.hpp"
#include "mipchain.hpp"
#include "scenedesc.hpp"
#include "spritelight.hpp"
#include "texels.hpp"

namespace Rtx
{
    osg::ref_ptr<const osg::Image> openImage(Resource::ImageManager& images, const VFS::Path::NormalizedView path)
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
        /// What a texture that could not be read is drawn as: mid grey and not magenta, because a
        /// live graph's unreadable textures are mostly things that were never files, and
        /// `getUnreadable` already reports them. One opaque BC1 block with both endpoints the same
        /// grey.
        TextureData standIn(std::vector<MipLevel>& levels)
        {
            // 0x8410 is RGB565 for (16, 16, 16) out of (31, 63, 31) — a touch above half, which is
            // mid grey once the sRGB curve is undone.
            static constexpr std::array<std::byte, 8> sBlock{ std::byte{ 0x10 }, std::byte{ 0x84 }, std::byte{ 0x10 },
                std::byte{ 0x84 }, std::byte{}, std::byte{}, std::byte{}, std::byte{} };

            const std::size_t first = levels.size();
            levels.push_back(MipLevel{ 0, 4, 4 });

            return TextureData{
                .mSource = TextureSource::StandIn,
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
        const TextureFormat format = readFormat(image);
        if (!isUploadable(format))
            throw InputError("texture \"" + image.getFileName() + "\" is " + std::string(nameOf(format)) + " ("
                + std::to_string(image.getPixelFormat()) + "), which is not one this renderer uploads");

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
            .mFormat = format,
            .mWidth = width,
            .mHeight = height,
            .mBytes
            = std::span(reinterpret_cast<const std::byte*>(image.data()), image.getTotalSizeInBytesIncludingMipmaps()),
            .mLevels = std::span<const MipLevel>(levels).subspan(first, count),
            .mName = image.getFileName(),
        };
    }

    void SceneTextures::describeAll(
        const SceneDesc& scene, Resource::ImageManager& images, const CompositeQueue* composites)
    {
        mEverything.resize(scene.textures().getRows().size());
        std::iota(mEverything.begin(), mEverything.end(), Index{ 0 });

        describe(scene, images, mEverything, composites);
    }

    void SceneTextures::describe(const SceneDesc& scene, Resource::ImageManager& images, std::span<const Index> slots,
        const CompositeQueue* composites)
    {
        mLevels.clear();
        mDescriptions.clear();
        mKept.clear();
        mUnreadable = 0;

        mKept.reserve(slots.size());

        for (const Index slot : slots)
        {
            // A free slot is not a texture. `SceneDesc` empties one the last thing naming it
            // gave back and leaves it in the table until something takes it over; describing it
            // would build an image, a shading map and a descriptor write for a slot no material can
            // reach — and count it as a texture that arrived.
            if (scene.textures().isFree(slot))
                continue;

            // A slot this renderer made rather than opened has no file to be asked for, and the
            // entry still has to exist because the description below is built from it. A composite
            // the queue has not finished is passed over the same way.

            osg::ref_ptr<const osg::Image> image;
            Index bakedFrom = sNoIndex;

            const TextureRow& row = scene.textures().getRows()[slot];
            if (row.mKind == TextureKind::File)
                image = openImage(images, row.mPath);
            else if (const std::optional<VFS::Path::Normalized> source = SpriteLightMap::sourceOf(row.mBaked))
            {
                // Made on the device from the sprite texture's own slot, which the emitter holds
                // beside this one: a bake carries no bytes and is shaped like its source there.
                // A source the table no longer holds is a bake of nothing, and it gets the stand-in
                // below like a file that could not be read.
                bakedFrom = scene.textures().findFile(*source);
            }

            mKept.push_back(Kept{ .mSlot = slot, .mBakedFrom = bakedFrom, .mImage = std::move(image) });
        }

        // Reserved before anything points into it, and that is what makes the spans safe. Every
        // description spans this one table, so it must not grow while they are being taken — and
        // every level count is known before the first description is built. A table kept from the
        // last arrival is usually large enough already, and then this asks for nothing.
        std::size_t levels = 0;
        for (const Kept& kept : mKept)
            levels += kept.mImage != nullptr ? kept.mImage->getNumMipmapLevels() : 1u;
        mLevels.reserve(levels);

        // What the assertion below is taken against: the reserve and the fill agree by argument
        // through branches that push a different number of levels each, and a growth is the failure.
        [[maybe_unused]] const std::size_t reserved = mLevels.capacity();

        mDescriptions.reserve(mKept.size());
        for (const Kept& kept : mKept)
        {
            std::optional<TextureData> described;
            if (kept.mImage != nullptr)
            {
                try
                {
                    described = describeImage(*kept.mImage, mLevels);

                    // What the file did not carry, the device makes. `MipChain` says why almost
                    // nothing in the game needs this and why the rain does.
                    described->mCompleteChain = MipChain::wantedFor(*described);
                }
                catch (const InputError&)
                {
                    described.reset();
                }
            }
            else if (kept.mBakedFrom != sNoIndex)
            {
                described = TextureData{
                    .mSource = TextureSource::SpriteBake,
                    .mFrom = kept.mBakedFrom,
                    .mFormat = TextureFormat::Rgba8Unorm,
                };
            }
            else if (const Index chunk = composites != nullptr ? composites->find(kept.mSlot) : sNoIndex;
                     chunk != sNoIndex)
            {
                // Flattened on the device in the placement after this arrival, from the chunk's
                // own stack: the description carries the chunk and no bytes.
                described = TextureData{
                    .mSource = TextureSource::GroundComposite,
                    .mFrom = chunk,
                    .mFormat = TextureFormat::Rgba8Srgb,
                };
            }

            if (!described.has_value())
            {
                ++mUnreadable;

                // Named rather than tallied, because a count says a texture is grey and nothing
                // about which one. Whichever of the two named the slot, or a composite that could
                // not be flattened reports itself as a file with no name.
                const TextureRow& row = scene.textures().getRows()[kept.mSlot];
                Log(Debug::Warning) << "Texture \"" << (row.mKind == TextureKind::File ? row.mPath.value() : row.mBaked)
                                    << "\" could not be read; drawing the stand-in";

                described = standIn(mLevels);
            }

            described->mSlot = kept.mSlot;
            described->mWrap = scene.textures().getRows()[kept.mSlot].mWrap;
            mDescriptions.push_back(*described);
        }

        assert(mLevels.capacity() == reserved && "the level table grew while descriptions spanned it");
    }
}

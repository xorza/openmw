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

#include <components/resource/imagemanager.hpp>
#include <components/vfs/pathutil.hpp>

#include "compositequeue.hpp"
#include "mipchain.hpp"
#include "refusals.hpp"
#include "scenedesc.hpp"
#include "spritelight.hpp"
#include "texels.hpp"
#include "texturetable.hpp"

namespace Rtx
{
    Result<osg::ref_ptr<const osg::Image>, std::string> openImage(
        Resource::ImageManager& images, const VFS::Path::NormalizedView path)
    {
        osg::ref_ptr<const osg::Image> image;
        try
        {
            image = images.getImage(path);
        }
        catch (const std::exception& failed)
        {
            return Err{ std::string(failed.what()) };
        }

        // The manager answers a file it cannot read with its warning image, having logged why:
        // an image, and not the one the file holds.
        if (image == nullptr || image == images.getWarningImage())
            return Err{ "no image reads from the file" };

        return image;
    }

    namespace
    {
        /// What a texture that could not be read is drawn as: mid grey and not magenta, because a
        /// live graph's unreadable textures are mostly things that were never files, and the refusal
        /// already names each. One opaque BC1 block with both endpoints the same grey.
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

    Result<void, std::string> checkUploadable(const osg::Image& image)
    {
        const TextureFormat format = readFormat(image);
        if (!isUploadable(format))
            return Err{ "its format is " + std::string(nameOf(format)) + " (" + std::to_string(image.getPixelFormat())
                + "), which this renderer does not upload" };

        if (!image.valid() || image.s() < 0 || image.t() < 0)
            return Err{ "it is " + std::to_string(image.s()) + " by " + std::to_string(image.t())
                + " texels, which no device holds" };

        return {};
    }

    Result<TextureData, std::string> describeImage(const osg::Image& image, std::vector<MipLevel>& levels)
    {
        if (const Result<void, std::string> uploadable = checkUploadable(image); !uploadable.isOk())
            return Err{ uploadable.error() };

        const TextureFormat format = readFormat(image);
        const auto width = static_cast<std::uint32_t>(image.s());
        const auto height = static_cast<std::uint32_t>(image.t());

        // As many as the file carries, and no more than reach a single texel: a header may count
        // levels past the last one, and a device takes no image with more levels than its size has.
        const std::size_t first = levels.size();
        const std::uint32_t count = std::min(image.getNumMipmapLevels(), levelsTo1x1(width, height));
        for (std::uint32_t level = 0; level < count; ++level)
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
        mRefusals.clear();

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
            // entry still has to exist because the description below is built from it.
            Kept kept{ .mSlot = slot };

            const TextureRow& row = scene.textures().getRows()[slot];
            if (row.mKind == TextureKind::File)
                kept.mImage = openImage(images, row.mPath);
            else if (const std::optional<VFS::Path::Normalized> source = SpriteLightMap::sourceOf(row.mBaked))
            {
                // Made on the device from the sprite texture's own slot, which the emitter holds
                // beside this one: a bake carries no bytes and is shaped like its source there.
                kept.mBakedFrom = scene.textures().findFile(*source);
            }

            mKept.push_back(std::move(kept));
        }

        // Reserved before anything points into it, and that is what makes the spans safe. Every
        // description spans this one table, so it must not grow while they are being taken — and
        // every level count is known before the first description is built. A table kept from the
        // last arrival is usually large enough already, and then this asks for nothing.
        std::size_t levels = 0;
        for (const Kept& kept : mKept)
            levels += kept.mImage.isOk() && kept.mImage.value() != nullptr ? kept.mImage.value()->getNumMipmapLevels()
                                                                           : 1u;
        mLevels.reserve(levels);

        // What the assertion below is taken against: the reserve and the fill agree by argument
        // through branches that push a different number of levels each, and a growth is the failure.
        [[maybe_unused]] const std::size_t reserved = mLevels.capacity();

        mDescriptions.reserve(mKept.size());
        for (const Kept& kept : mKept)
        {
            const TextureRow& row = scene.textures().getRows()[kept.mSlot];

            const Result<TextureData, std::string> described = describeKept(kept, composites);
            TextureData data;
            if (described.isOk())
                data = described.value();
            else
            {
                // Whichever of the two named the slot.
                mRefusals.push_back(Refusal{ .mKind = Refused::Texture,
                    .mName = std::string(row.mKind == TextureKind::File ? row.mPath.value() : row.mBaked),
                    .mWhy = described.error() });
                data = standIn(mLevels);
            }

            data.mSlot = kept.mSlot;
            data.mWrap = row.mWrap;
            mDescriptions.push_back(data);
        }

        assert(mLevels.capacity() == reserved && "the level table grew while descriptions spanned it");

        // The array's own limit, met where a texture was added rather than here, and reported with
        // the rest of what an arrival stood in for: one refusal for all of them, because what they
        // share is the limit.
        if (scene.textures().getRefused() > 0)
            mRefusals.push_back(Refusal{ .mKind = Refused::Texture,
                .mWhy = "past the " + std::to_string(TextureTable::sCapacity) + " textures the array holds" });
    }

    Result<TextureData, std::string> SceneTextures::describeKept(const Kept& kept, const CompositeQueue* composites)
    {
        if (!kept.mImage.isOk())
            return Err{ kept.mImage.error() };

        if (const osg::Image* image = kept.mImage.value().get())
        {
            const Result<TextureData, std::string> read = describeImage(*image, mLevels);
            if (!read.isOk())
                return read;

            // What the file did not carry, the device makes. `MipChain` says why almost nothing in
            // the game needs this and why the rain does.
            TextureData described = read.value();
            described.mCompleteChain = MipChain::wantedFor(described);
            return described;
        }

        if (kept.mBakedFrom.has_value())
        {
            // A source the table no longer holds is a bake of nothing.
            if (*kept.mBakedFrom == sNoIndex)
                return Err{ "the texture it bakes is no longer held" };

            return TextureData{
                .mSource = TextureSource::SpriteBake,
                .mFrom = *kept.mBakedFrom,
                .mFormat = TextureFormat::Rgba8Unorm,
            };
        }

        // Flattened on the device in the placement after this arrival, from the chunk's own stack:
        // the description carries the chunk and no bytes.
        const Index chunk = composites != nullptr ? composites->find(kept.mSlot) : sNoIndex;
        if (chunk == sNoIndex)
            return Err{ "no ground was queued to flatten into it" };

        return TextureData{
            .mSource = TextureSource::GroundComposite,
            .mFrom = chunk,
            .mFormat = TextureFormat::Rgba8Srgb,
        };
    }
}

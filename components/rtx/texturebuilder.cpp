#include "texturebuilder.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <osg/Image>
#include <osg/ref_ptr>

#include <components/crashcatcher/crash.hpp>
#include <components/crashcatcher/crashnote.hpp>
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
    namespace
    {
        /// Why a file slot has nothing to upload: the adder found no image at its path.
        constexpr std::string_view sNoImage = "no image reads from the file";

        /// How many of an image's levels a description keeps: as many as the file carries, and no
        /// more than reach a single texel, because a header may count levels past the last one and
        /// a device takes no image with more levels than its size has.
        std::uint32_t keptLevels(const osg::Image& image)
        {
            return std::min(image.getNumMipmapLevels(),
                levelsTo1x1(static_cast<std::uint32_t>(image.s()), static_cast<std::uint32_t>(image.t())));
        }

        /// How many bits each channel of a sixteen-bit format takes, from the high bit down: alpha,
        /// red, green and blue. Nought alpha bits is an opaque texel.
        struct ChannelBits
        {
            std::uint32_t mAlpha = 0;
            std::uint32_t mRed = 0;
            std::uint32_t mGreen = 0;
            std::uint32_t mBlue = 0;
        };

        ChannelBits channelBitsOf(TextureFormat format)
        {
            switch (format)
            {
                case TextureFormat::Rgb565:
                    return ChannelBits{ .mRed = 5, .mGreen = 6, .mBlue = 5 };
                case TextureFormat::Argb1555:
                    return ChannelBits{ .mAlpha = 1, .mRed = 5, .mGreen = 5, .mBlue = 5 };
                case TextureFormat::Xrgb1555:
                    return ChannelBits{ .mRed = 5, .mGreen = 5, .mBlue = 5 };
                case TextureFormat::Argb4444:
                    return ChannelBits{ .mAlpha = 4, .mRed = 4, .mGreen = 4, .mBlue = 4 };
                case TextureFormat::Xrgb4444:
                    return ChannelBits{ .mRed = 4, .mGreen = 4, .mBlue = 4 };
                default:
                    break;
            }

            Crash::fatal("a format widened that is not sixteen bits a texel");
        }

        /// The byte nearest what a device samples from a `bits`-bit unsigned normalized channel,
        /// `value / (2^bits - 1)`. Not the bits repeated down the byte, which a BC1 endpoint is
        /// decoded by and which lands a step off that for fourteen of the five- and six-bit values.
        std::byte widenChannel(std::uint32_t value, std::uint32_t bits)
        {
            const std::uint32_t top = (1u << bits) - 1;
            return static_cast<std::byte>((value * 255 + top / 2) / top);
        }

        /// Every texel of `from`, little-endian sixteen-bit words of `format`, into `into` as RGBA8.
        void widen(TextureFormat format, std::span<const std::byte> from, std::span<std::byte> into)
        {
            const ChannelBits bits = channelBitsOf(format);
            const std::uint32_t greenAt = bits.mBlue;
            const std::uint32_t redAt = greenAt + bits.mGreen;
            const std::uint32_t alphaAt = redAt + bits.mRed;
            const auto field = [](std::uint32_t word, std::uint32_t at, std::uint32_t width) {
                return (word >> at) & ((1u << width) - 1);
            };

            assert(into.size() == from.size() * 2 && "RGBA8 is twice a sixteen-bit texel");
            for (std::size_t texel = 0; texel < from.size() / 2; ++texel)
            {
                const std::uint32_t word = std::to_integer<std::uint32_t>(from[texel * 2])
                    | std::to_integer<std::uint32_t>(from[texel * 2 + 1]) << 8;
                std::byte* out = &into[texel * 4];
                out[0] = widenChannel(field(word, redAt, bits.mRed), bits.mRed);
                out[1] = widenChannel(field(word, greenAt, bits.mGreen), bits.mGreen);
                out[2] = widenChannel(field(word, 0, bits.mBlue), bits.mBlue);
                out[3] = bits.mAlpha > 0 ? widenChannel(field(word, alphaAt, bits.mAlpha), bits.mAlpha)
                                         : std::byte{ 0xFF };
            }
        }

        /// Whether the first slices of an image's kept levels lie back to back, which is what a
        /// description spans. A volume's levels are each every slice of it, so from its second
        /// level on they do not.
        bool slicesAdjoin(const osg::Image& image)
        {
            return image.r() == 1 || keptLevels(image) == 1;
        }

        /// How many bytes `describeImage` lays into its `texels` for `image`, which is what a
        /// caller holding earlier descriptions reserves first: a sixteen-bit format widened, or a
        /// volume's first slices gathered. Nought for an image spanned where it is, and for one it
        /// refuses, whose format may have no layout to count by.
        std::size_t laidBytes(const osg::Image& image, TextureEncoding encoding)
        {
            const TextureFormat format = readFormat(image, encoding);
            const bool widened = isWidened(format);
            if (!widened && (!isUploadable(format) || slicesAdjoin(image)))
                return 0;
            if (image.s() <= 0 || image.t() <= 0)
                return 0;

            const auto width = static_cast<std::uint32_t>(image.s());
            const auto height = static_cast<std::uint32_t>(image.t());
            const TexelLayout laid = layoutOf(widened ? TextureFormat::Rgba8Unorm : format);

            std::size_t bytes = 0;
            for (std::uint32_t level = 0; level < keptLevels(image); ++level)
                bytes += laid.levelBytes(std::max(width >> level, 1u), std::max(height >> level, 1u));

            return bytes;
        }
    }

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
            return Err{ std::string(sNoImage) };

        return image;
    }

    TextureData describeStandIn()
    {
        // Mid grey and not magenta, because a live graph's unreadable textures are mostly things
        // that were never files, and the refusal already names each. Only a base colour draws it;
        // every other reader reads the slot as none, `TEXTURE_STANDS_IN`. One opaque BC1 block with both
        // endpoints the same grey: 0x8410 is RGB565 for (16, 16, 16) out of (31, 63, 31) — a touch
        // above half, which is mid grey once the sRGB curve is undone.
        static constexpr std::array<std::byte, 8> sBlock{ std::byte{ 0x10 }, std::byte{ 0x84 }, std::byte{ 0x10 },
            std::byte{ 0x84 }, std::byte{}, std::byte{}, std::byte{}, std::byte{} };
        static constexpr MipLevel sLevel{ .mOffset = 0, .mWidth = 4, .mHeight = 4 };

        return TextureData{
            .mSource = TextureSource::StandIn,
            .mFormat = TextureFormat::Bc1RgbaSrgb,
            .mWidth = 4,
            .mHeight = 4,
            .mBytes = sBlock,
            .mLevels = std::span<const MipLevel>(&sLevel, 1),
            .mName = "stand-in",
        };
    }

    Result<void, std::string> checkUploadable(const osg::Image& image, const TextureEncoding encoding)
    {
        const TextureFormat format = readFormat(image, encoding);
        if (!isUploadable(format) && !isWidened(format))
            return Err{ "its format is " + std::string(nameOf(format)) + " (" + std::to_string(image.getPixelFormat())
                + "), which this renderer does not upload" };

        if (!image.valid() || image.s() < 0 || image.t() < 0)
            return Err{ "it is " + std::to_string(image.s()) + " by " + std::to_string(image.t())
                + " texels, which no device holds" };

        return {};
    }

    Result<TextureData, std::string> describeImage(const osg::Image& image, std::vector<MipLevel>& levels,
        std::vector<std::byte>& texels, const TextureEncoding encoding)
    {
        // Every reader of an image's bytes on the processor comes through here first, so a crash
        // in one names the file.
        const Crash::NoteScope noted("describing the texture \"{}\"", image.getFileName());

        if (const Result<void, std::string> uploadable = checkUploadable(image, encoding); !uploadable.isOk())
            return Err{ uploadable.error() };

        const TextureFormat format = readFormat(image, encoding);
        const TexelLayout layout = layoutOf(format);
        const auto width = static_cast<std::uint32_t>(image.s());
        const auto height = static_cast<std::uint32_t>(image.t());

        // **The layout held against OpenSceneGraph's before a byte is read.** Every reader of the
        // bytes below and the upload take a level's size from `layout`, and the image's buffer is
        // what its loader allocated level by level, `computeImageSizeInBytes`; where the two count
        // a level differently, reading the image by `layout` runs past its buffer or short of its
        // levels. A format read by its pixel format alone did that to every sixteen-bit file — and
        // a padded row would do it again. Level by level and not by `getTotalSizeInBytes`, whose
        // count of a block format with no chain differs between OpenSceneGraph builds: a two-by-two
        // BC3 is four bytes of its sixteen in one and 32 in another. The bytes are the kept levels'
        // and no more, so an upload stages none of what they leave out.
        //
        // **A volume is its first slice**, as the rasterizer draws a file bound as a flat texture:
        // each level is read at its own offset for one slice, and every slice counts toward where
        // the next level begins. A player's crash in the staging copy was a 128 by 128 DXT3 file of
        // four slices staged at all four.
        const std::size_t first = levels.size();
        const std::uint32_t count = keptLevels(image);
        const auto depth = static_cast<std::uint32_t>(image.r());
        std::size_t kept = 0;
        std::size_t laid = 0;
        for (std::uint32_t level = 0; level < count; ++level)
        {
            const std::uint32_t across = std::max(width >> level, 1u);
            const std::uint32_t down = std::max(height >> level, 1u);
            const std::size_t ours = layout.levelBytes(across, down);
            const auto theirs = [&](std::uint32_t slices) -> std::size_t {
                return osg::Image::computeImageSizeInBytes(static_cast<int>(across), static_cast<int>(down),
                    static_cast<int>(slices), image.getPixelFormat(), image.getDataType(), image.getPacking());
            };

            if (image.getMipmapOffset(level) != laid || ours != theirs(1))
            {
                levels.resize(first);
                return Err{ "its level " + std::to_string(level) + " is " + std::to_string(theirs(1))
                    + " bytes at byte " + std::to_string(image.getMipmapOffset(level)) + ", where "
                    + std::string(nameOf(format)) + " at " + std::to_string(across) + " by " + std::to_string(down)
                    + " is " + std::to_string(ours) + " at byte " + std::to_string(laid) };
            }

            levels.push_back(
                MipLevel{ .mOffset = static_cast<std::uint32_t>(kept), .mWidth = across, .mHeight = down });
            kept += ours;
            laid += theirs(std::max(depth >> level, 1u));
        }

        const auto* const data = reinterpret_cast<const std::byte*>(image.data());
        TextureData described{
            .mFormat = format,
            .mEncoding = encoding,
            .mWidth = width,
            .mHeight = height,
            .mBytes = std::span(data, kept),
            .mLevels = std::span<const MipLevel>(levels).subspan(first, count),
            .mName = image.getFileName(),
        };
        const bool widened = isWidened(format);
        if (!widened && slicesAdjoin(image))
            return described;

        // **Laid into `texels`, which the caller holds**, so the description spans storage that
        // outlives this call as the image's own does: level by level from where each lies in the
        // image, widened where the format is sixteen bits a texel. Widened is twice the bytes,
        // because RGBA8 is four bytes a texel, and so every level begins at twice the offset.
        const std::size_t scale = widened ? 2 : 1;
        const std::size_t from = texels.size();
        texels.resize(from + kept * scale);
        const std::span<std::byte> into = std::span(texels).subspan(from);
        for (std::uint32_t level = 0; level < count; ++level)
        {
            MipLevel& laidOut = levels[first + level];
            const std::size_t bytes = layout.levelBytes(laidOut.mWidth, laidOut.mHeight);
            const std::span<const std::byte> source(data + image.getMipmapOffset(level), bytes);
            const std::span<std::byte> target = into.subspan(laidOut.mOffset * scale, bytes * scale);
            if (widened)
                widen(format, source, target);
            else
                std::copy(source.begin(), source.end(), target.begin());

            laidOut.mOffset *= static_cast<std::uint32_t>(scale);
        }

        if (widened)
            described.mFormat
                = encoding == TextureEncoding::Colour ? TextureFormat::Rgba8Srgb : TextureFormat::Rgba8Unorm;
        described.mBytes = std::span<const std::byte>(texels).subspan(from, kept * scale);
        return described;
    }

    void SceneTextures::describeAll(const SceneDesc& scene, const CompositeQueue* composites)
    {
        mEverything.resize(scene.textures().getRows().size());
        std::iota(mEverything.begin(), mEverything.end(), Index{ 0 });

        describe(scene, mEverything, composites);
    }

    void SceneTextures::describe(const SceneDesc& scene, std::span<const Index> slots, const CompositeQueue* composites)
    {
        mLevels.clear();
        mTexels.clear();
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
            const TextureRow& row = scene.textures().getRows()[slot];
            Kept kept{ .mSlot = slot, .mEncoding = row.mEncoding };

            // The image the adder held, and never the file opened again: this runs on the frame an
            // arrival lands on, and a path is a lock and a disk read.
            if (row.mKind == TextureKind::File && row.mImage != nullptr)
                kept.mImage = row.mImage;
            else if (row.mKind == TextureKind::File)
                kept.mImage = Err{ std::string(sNoImage) };
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
        // last arrival is usually large enough already, and then this asks for nothing. Only a file
        // puts levels here: a bake and a composite carry none, and the stand-in's are its own.
        std::size_t levels = 0;
        std::size_t texels = 0;
        for (const Kept& kept : mKept)
            if (kept.mImage.isOk() && kept.mImage.value() != nullptr)
            {
                levels += kept.mImage.value()->getNumMipmapLevels();
                texels += laidBytes(*kept.mImage.value(), kept.mEncoding);
            }
        mLevels.reserve(levels);
        mTexels.reserve(texels);

        // What the assertions below are taken against: the reserve and the fill agree by argument
        // through branches that push a different number of levels each, and a growth is the failure.
        [[maybe_unused]] const std::size_t reserved = mLevels.capacity();
        [[maybe_unused]] const std::size_t reservedTexels = mTexels.capacity();

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
                data = describeStandIn();
            }

            data.mSlot = kept.mSlot;
            data.mWrap = row.mWrap;
            mDescriptions.push_back(data);
        }

        assert(mLevels.capacity() == reserved && "the level table grew while descriptions spanned it");
        assert(mTexels.capacity() == reservedTexels && "the laid texels grew while descriptions spanned them");

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
            const Result<TextureData, std::string> read = describeImage(*image, mLevels, mTexels, kept.mEncoding);
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
        const CompositeQueue::Baked chunk
            = composites != nullptr ? composites->find(kept.mSlot) : CompositeQueue::Baked{};
        if (chunk.mMaterial == sNoIndex)
            return Err{ "no ground was queued to flatten into it" };

        if (chunk.mGloss)
            return TextureData{
                .mSource = TextureSource::GroundGloss,
                .mFrom = chunk.mMaterial,
                .mFormat = TextureFormat::Rgba8Unorm,
                .mEncoding = TextureEncoding::Data,
            };

        return TextureData{
            .mSource = TextureSource::GroundComposite,
            .mFrom = chunk.mMaterial,
            .mFormat = TextureFormat::Rgba8Srgb,
        };
    }
}

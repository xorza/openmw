#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "contract.hpp"
#include "runs.hpp"
#include "textureencoding.hpp"
#include "texturewrap.hpp"

namespace Rtx
{
    /// Where one mip level sits in a texture's bytes, and how big it is.
    struct MipLevel
    {
        std::uint32_t mOffset = 0;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
    };

    /// How many levels a chain from `width` by `height` down to one texel has: what
    /// `MipPyramid::layOutTo1x1` lays out, for an image made to hold one.
    inline std::uint32_t levelsTo1x1(std::uint32_t width, std::uint32_t height)
    {
        return static_cast<std::uint32_t>(std::bit_width(std::max(width, height)));
    }

    /// The shape of a chain of mip levels: where each one sits and how big it is. The shape and not
    /// the texels, because four payloads build the same chain.
    struct MipPyramid
    {
        std::vector<MipLevel> mLevels;

        void reuse() { mLevels.clear(); }

        bool isEmpty() const { return mLevels.empty(); }

        std::uint32_t getLevelCount() const { return static_cast<std::uint32_t>(mLevels.size()); }

        const MipLevel& getLevel(std::uint32_t level) const
        {
            assert(level < mLevels.size() && "a level past the end of the chain");
            return mLevels[level];
        }

        std::uint32_t getWidth() const { return mLevels.empty() ? 0 : mLevels.front().mWidth; }
        std::uint32_t getHeight() const { return mLevels.empty() ? 0 : mLevels.front().mHeight; }

        /// Where texel `(x, y)` of `level` begins, in a payload of `stride` bytes a texel.
        std::size_t offsetOf(std::uint32_t level, std::uint32_t x, std::uint32_t y, std::size_t stride) const
        {
            const MipLevel& which = getLevel(level);
            assert(x < which.mWidth && y < which.mHeight && "a texel outside its level");

            return which.mOffset + (std::size_t{ y } * which.mWidth + x) * stride;
        }

        /// Lays out a chain from `width` by `height` down to one texel, and answers how many bytes
        /// it needs at `stride` bytes a texel. Every level's offset is in that payload.
        std::size_t layOutTo1x1(std::uint32_t width, std::uint32_t height, std::size_t stride);

        /// Lays out one level per entry of `shape`, keeping their extents and renumbering their
        /// offsets into a payload of `stride` bytes a texel, because the source's offsets are in
        /// the source's payload. Answers how many bytes that needs.
        std::size_t layOutLike(std::span<const MipLevel> shape, std::size_t stride);
    };

    /// Every format OpenSceneGraph decodes a texture into: the ones this renderer uploads first,
    /// then the ones it widens to one of those on the way in, then the ones it only counts, because
    /// a file the uploader refuses is still a file the report has to name. A colour's formats are
    /// sRGB, because the files hold display-encoded bytes and the hardware converts inside the
    /// filter; data's are the same blocks read linearly (`TextureEncoding`). `readFormat` names one
    /// by the image's pixel format and its data type together, because the pixel format alone says
    /// which channels and not how many bytes they take.
    enum class TextureFormat : std::uint8_t
    {
        /// BC1 with its punch-through alpha bit read. Both DXT1 spellings land here, and
        /// `describeImage` says why the header's alpha flag is not consulted.
        Bc1RgbaSrgb,
        Bc2Srgb,
        Bc3Srgb,

        /// Uncompressed, and not sRGB: a data map stored loose, and what a test asserting an exact
        /// texel needs, because a block cannot express an arbitrary value and an sRGB format would
        /// land the assertion on the far side of a transfer function.
        Rgba8Unorm,

        /// Uncompressed and display-encoded, in the two channel orders a `.dds` states them in. The
        /// cloud decks are 32-bit `DDPF_RGB`, and a renderer that takes only blocks draws every
        /// weather grey. Both orders rather than one and a swizzle, because converting would mean
        /// owning a copy of a buffer this type is defined by not owning.
        Rgba8Srgb,
        Bgra8Srgb,

        /// The same blocks and orders as data. A companion map is BC1 or BC3 nearly always.
        Bc1RgbaUnorm,
        Bc2Unorm,
        Bc3Unorm,
        Bgra8Unorm,

        /// Two channels, as OpenMW takes a normal map with its Z left out. Data only: a two-channel
        /// file bound as a colour has lost its blue, and is refused as a colour.
        Bc5Unorm,

        /// Sixteen bits a texel, as the old mods' `.dds` files hold them, in the channel order the
        /// file states from the high bit down: `describeImage` widens each to RGBA8 in the encoding
        /// the slot asks for, because a device's sixteen-bit formats have no sRGB spelling. The `X`
        /// ones carry a bit the file leaves undefined, and are opaque.
        Rgb565,
        Argb1555,
        Xrgb1555,
        Argb4444,
        Xrgb4444,

        /// Read by the census and never uploaded: `describeImage` refuses them by name, and does
        /// not write the missing channels in as it widens the sixteen-bit ones.
        Rgb8,
        Luminance,
        LuminanceAlpha,

        /// Anything else, and there is one count of them rather than one each.
        Unnamed,
    };

    inline constexpr std::size_t sTextureFormatCount = static_cast<std::size_t>(TextureFormat::Unnamed) + 1;

    /// Whether a backend takes a format as it is — every one before `Rgb565`.
    inline bool isUploadable(const TextureFormat format)
    {
        return format < TextureFormat::Rgb565;
    }

    /// Whether `describeImage` widens a format to RGBA8 on the way in.
    inline bool isWidened(const TextureFormat format)
    {
        return format >= TextureFormat::Rgb565 && format < TextureFormat::Rgb8;
    }

    /// How a format lays its texels out: square blocks `mSide` texels across of `mBytes` bytes
    /// each, rows tight. A loose format is a block of one texel.
    ///
    /// **The one statement of how many bytes a texel takes**, which the level arithmetic, the
    /// upload and every reader of a description's bytes go through. `describeImage` holds it
    /// against what OpenSceneGraph says of the same image, so a format whose bytes the two count
    /// differently is refused by name before any of them is read.
    struct TexelLayout
    {
        std::uint32_t mSide = 1;
        std::uint32_t mBytes = 0;

        bool isBlocked() const { return mSide > 1; }

        /// How many bytes a level `width` by `height` texels takes.
        std::size_t levelBytes(std::uint32_t width, std::uint32_t height) const
        {
            return std::size_t{ (width + mSide - 1) / mSide } * ((height + mSide - 1) / mSide) * mBytes;
        }
    };

    /// Exhaustive rather than defaulted, so a format added to the enum is a build failure here
    /// rather than a format read at some other one's size. `Unnamed` has none: nothing knows it.
    inline TexelLayout layoutOf(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::Bc1RgbaSrgb:
            case TextureFormat::Bc1RgbaUnorm:
                return TexelLayout{ .mSide = 4, .mBytes = 8 };
            case TextureFormat::Bc2Srgb:
            case TextureFormat::Bc3Srgb:
            case TextureFormat::Bc2Unorm:
            case TextureFormat::Bc3Unorm:
            case TextureFormat::Bc5Unorm:
                return TexelLayout{ .mSide = 4, .mBytes = 16 };
            case TextureFormat::Rgba8Unorm:
            case TextureFormat::Rgba8Srgb:
            case TextureFormat::Bgra8Srgb:
            case TextureFormat::Bgra8Unorm:
                return TexelLayout{ .mBytes = 4 };
            case TextureFormat::Rgb565:
            case TextureFormat::Argb1555:
            case TextureFormat::Xrgb1555:
            case TextureFormat::Argb4444:
            case TextureFormat::Xrgb4444:
            case TextureFormat::LuminanceAlpha:
                return TexelLayout{ .mBytes = 2 };
            case TextureFormat::Rgb8:
                return TexelLayout{ .mBytes = 3 };
            case TextureFormat::Luminance:
                return TexelLayout{ .mBytes = 1 };
            case TextureFormat::Unnamed:
                break;
        }

        broken("a texture format with no layout");
    }

    /// Whether a loose format states its colours blue first, which every reader of its bytes has
    /// to know to read red as red.
    inline bool isBgr(TextureFormat format)
    {
        return format == TextureFormat::Bgra8Srgb || format == TextureFormat::Bgra8Unorm;
    }

    /// Whether a format's bytes are display-encoded, which every colour format's are. A data format
    /// is not, and neither is `Rgba8Unorm`, which a test also uses: a value written into it is the
    /// value light transport sees, with no transfer function between the expectation and the answer.
    inline bool isSrgb(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::Bc1RgbaSrgb:
            case TextureFormat::Bc2Srgb:
            case TextureFormat::Bc3Srgb:
            case TextureFormat::Rgba8Srgb:
            case TextureFormat::Bgra8Srgb:
                return true;
            case TextureFormat::Rgba8Unorm:
            case TextureFormat::Bc1RgbaUnorm:
            case TextureFormat::Bc2Unorm:
            case TextureFormat::Bc3Unorm:
            case TextureFormat::Bgra8Unorm:
            case TextureFormat::Bc5Unorm:
            case TextureFormat::Rgb565:
            case TextureFormat::Argb1555:
            case TextureFormat::Xrgb1555:
            case TextureFormat::Argb4444:
            case TextureFormat::Xrgb4444:
            case TextureFormat::Rgb8:
            case TextureFormat::Luminance:
            case TextureFormat::LuminanceAlpha:
            case TextureFormat::Unnamed:
                return false;
        }

        broken("unknown texture format");
    }

    /// Whether a format is BC1, whose blocks carry a punch-through alpha in their endpoint order,
    /// in either encoding.
    inline bool isBc1(TextureFormat format)
    {
        return format == TextureFormat::Bc1RgbaSrgb || format == TextureFormat::Bc1RgbaUnorm;
    }

    /// What stands in a texture slot, which decides what a description carries and how a backend
    /// builds it. Stated rather than deduced from two sentinels, for the reason
    /// `Rtx::TextureKind` gives of the table a description is built from: a reader that has to
    /// work out which of four a value is works it out in its own order, and two orders are one
    /// rule written twice.
    enum class TextureSource : std::uint8_t
    {
        /// A file the content named, decoded. `TextureData::mBytes` and `mLevels` are the file's
        /// own, and `mCompleteChain` says whether the device finishes a chain it did not carry.
        File,

        /// The light bake of a sprite texture — `SpriteLightMap` says what a bake is — made on
        /// the device from that texture's alpha, `SpriteLightPass`. `TextureData::mFrom` is the
        /// slot it is made from, and it carries no bytes: a bake is shaped like its source.
        SpriteBake,

        /// A distant chunk's layer stack, flattened on the device by `GroundCompositePass` in the
        /// placement that writes the chunk's material row. `TextureData::mFrom` is that row, and
        /// it carries no bytes: a composite is `GROUND_COMPOSITE_EXTENT` square with a chain to
        /// one texel.
        GroundComposite,

        /// The same chunk's gloss, baked in the same pass from the same stack: how much of its
        /// ground reflects in red and how rough it is in green, as data. `TextureData::mFrom` is
        /// the chunk's row, and the image is shaped as its composite is.
        GroundGloss,

        /// What a slot is drawn as where what it names cannot stand: `describeStandIn`, whose
        /// bytes it carries. A backend stands one image for every such slot, and for a slot it has
        /// no room for, so a refusal costs the device nothing. Drawn only as a base colour: a reader
        /// of an optional map reads the slot as none, `TEXTURE_STANDS_IN`.
        StandIn,
    };

    /// A decoded texture, ready to upload and owning none of it. No graphics API in it, because an
    /// upload of a block-compressed file with its chain already built is a copy and never a
    /// conversion.
    struct TextureData
    {
        /// Which row of the backend's texture array this is, which is the slot a material holds.
        /// Carried rather than implied by position, because a slot a departing cell freed is taken
        /// over wherever it sits.
        Index mSlot = 0;

        /// How the slot is addressed past its edges, which is the sampler a backend binds it
        /// through. The scene's table says, per slot.
        TextureWrap mWrap = TextureWrap::Repeat;

        /// What stands in the slot, which says which of the fields below mean anything.
        TextureSource mSource = TextureSource::File;

        /// What the source names, by the source: the sprite texture a bake is made from, or the
        /// material row whose ground a composite or its gloss is. `sNoIndex` under `File` and
        /// `StandIn`, which are made from nothing but their own bytes.
        Index mFrom = sNoIndex;

        TextureFormat mFormat = TextureFormat::Bc1RgbaSrgb;

        /// What the slot's texels are, which the scene's table says per slot and `mFormat` follows.
        TextureEncoding mEncoding = TextureEncoding::Colour;

        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        /// Every level, back to back. The levels index into this. Empty under `SpriteBake`,
        /// `GroundComposite` and `GroundGloss`, whose bytes are the device's.
        std::span<const std::byte> mBytes;
        std::span<const MipLevel> mLevels;

        /// Whether a backend completes the chain the file did not carry, `MipChainPass`, from the
        /// one level here down to one texel. Set by the builder where `MipChain::wantedFor` says,
        /// and never for a texture a test paints to be read at its one level. Under `File` alone.
        bool mCompleteChain = false;

        /// What to call it in a capture — the file it came from. Spans storage the description's
        /// owner holds, like everything else here. Empty is allowed and only costs a nameless object
        /// in a debugger; every backend has somewhere to put it.
        std::string_view mName;

        /// Whether the shading map beside it is the neutral one rather than an estimate made off
        /// its texels — a composite, whose painted light came off per tile in the bake and would
        /// come off twice; a bake, which nothing divides; the stand-in, which is one grey; and data,
        /// which is no picture of anything lit. A colour file's is estimated on the device as it
        /// arrives, `ShadingPass`. Derived, because the source and the encoding decide it.
        bool hasNeutralShading() const { return mSource != TextureSource::File || mEncoding == TextureEncoding::Data; }

        /// The first level no wider and no taller than `side`, or nothing where every level is
        /// larger: where a texture held to that side begins. The levels halve, so every level
        /// after it is within the side too.
        std::optional<std::uint32_t> firstLevelWithin(std::uint32_t side) const
        {
            for (std::uint32_t level = 0; level < mLevels.size(); ++level)
                if (std::max(mLevels[level].mWidth, mLevels[level].mHeight) <= side)
                    return level;

            return std::nullopt;
        }

        /// How many bytes the levels from `level` on take: what an upload that begins there copies.
        std::size_t bytesFrom(std::uint32_t level) const
        {
            assert(level < mLevels.size() && "a level past the end of the chain");
            return mBytes.size() - mLevels[level].mOffset;
        }
    };

}

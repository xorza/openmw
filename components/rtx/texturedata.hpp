#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "error.hpp"
#include "runs.hpp"

namespace Rtx
{
    /// Where one mip level sits in a texture's bytes, and how big it is.
    struct MipLevel
    {
        std::uint32_t mOffset = 0;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
    };

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
    /// then the ones it only counts, because a file the uploader refuses is still a file the report
    /// has to name. The content formats are sRGB, because the files hold display-encoded bytes and
    /// the hardware converts inside the filter. Nothing uploads under `Unnamed`: `describeImage`
    /// refuses every format past `Bgra8Srgb` with the file's name in the message.
    enum class TextureFormat : std::uint8_t
    {
        /// BC1 with its punch-through alpha bit read. Both DXT1 spellings land here, and
        /// `describeImage` says why the header's alpha flag is not consulted.
        Bc1RgbaSrgb,
        Bc2Srgb,
        Bc3Srgb,

        /// Uncompressed, and deliberately not sRGB. No content file holds this — it is what a test
        /// asserting an exact texel needs, because a block cannot express an arbitrary value and an
        /// sRGB format would land the assertion on the far side of a transfer function.
        Rgba8Unorm,

        /// Uncompressed and display-encoded, in the two channel orders a `.dds` states them in. The
        /// cloud decks are 32-bit `DDPF_RGB`, and a renderer that takes only blocks draws every
        /// weather grey. Both orders rather than one and a swizzle, because converting would mean
        /// owning a copy of a buffer this type is defined by not owning.
        Rgba8Srgb,
        Bgra8Srgb,

        /// Read by the census and never uploaded. Three-channel and single-channel spellings are
        /// refused deliberately: uploading one would need the missing channels written in, which
        /// means owning a buffer, and nothing this renderer reads stores a texture without them.
        Rgb8,
        Luminance,
        LuminanceAlpha,

        /// Anything else, and there is one count of them rather than one each.
        Unnamed,
    };

    inline constexpr std::size_t sTextureFormatCount = static_cast<std::size_t>(TextureFormat::Unnamed) + 1;

    /// Whether this renderer uploads a format at all — the first six — or only counts it.
    inline bool isUploadable(const TextureFormat format)
    {
        return format < TextureFormat::Rgb8;
    }

    /// How many bytes one block of a format occupies, or zero where its texels are not blocked.
    /// Exhaustive rather than defaulted, so that a format added to the enum is a build failure
    /// here rather than a block format quietly read as though its texels were loose bytes.
    inline std::uint32_t blockBytes(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::Bc1RgbaSrgb:
                return 8;
            case TextureFormat::Bc2Srgb:
            case TextureFormat::Bc3Srgb:
                return 16;
            case TextureFormat::Rgba8Unorm:
            case TextureFormat::Rgba8Srgb:
            case TextureFormat::Bgra8Srgb:
            case TextureFormat::Rgb8:
            case TextureFormat::Luminance:
            case TextureFormat::LuminanceAlpha:
            case TextureFormat::Unnamed:
                return 0;
        }

        throw Error("unknown texture format");
    }

    /// Whether a format's bytes are display-encoded, which every content format's are. The one
    /// that is not exists for tests: a value written into an `Rgba8Unorm` texture is the value
    /// light transport sees, with no transfer function between the expectation and the answer.
    inline bool isSrgb(TextureFormat format)
    {
        return format != TextureFormat::Rgba8Unorm;
    }

    /// A decoded texture, ready to upload and owning none of it. No graphics API in it, because an
    /// upload of a block-compressed file with its chain already built is a copy and never a
    /// conversion.
    struct TextureData
    {
        /// Which row of the backend's texture array this is, which is the slot a material holds.
        /// Carried rather than implied by position, because a slot a departing cell freed is taken
        /// over wherever it sits.
        Index mSlot = 0;

        TextureFormat mFormat = TextureFormat::Bc1RgbaSrgb;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        /// Every level, back to back. The levels index into this.
        std::span<const std::byte> mBytes;
        std::span<const MipLevel> mLevels;

        /// The light already painted into it, as `SHADING_EXTENT` squared factors to divide out.
        /// Empty where nothing estimated one, which the shader reads as neutral.
        std::span<const float> mShading;

        /// What to call it in a capture — the file it came from. Spans storage the description's
        /// owner holds, like everything else here. Empty is allowed and only costs a nameless object
        /// in a debugger; every backend has somewhere to put it.
        std::string_view mName;
    };

}

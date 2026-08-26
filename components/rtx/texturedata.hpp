#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "error.hpp"

namespace Rtx
{
    /// Where one mip level sits in a texture's bytes, and how big it is.
    struct MipLevel
    {
        std::uint32_t mOffset = 0;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
    };

    /// Every format this renderer uploads.
    ///
    /// The content formats are sRGB, because that is what the files hold: Morrowind's textures were
    /// authored and stored display-encoded, and sampling them as sRGB is what hands the shader the
    /// linear values light transport has to be done in — free, because the hardware converts inside
    /// the filter.
    ///
    /// **There is no `Undefined`.** A file this cannot read is a content error carrying a message,
    /// not a value that travels one more step before anyone notices it.
    enum class TextureFormat
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

        /// Uncompressed and display-encoded, in the two channel orders a `.dds` states them in.
        ///
        /// **Morrowind's meshes are block-compressed to the last file and its sky is not.** The
        /// cloud decks are 32-bit `DDPF_RGB`, which is what a texture painted for a full-screen dome
        /// in 2002 would be, and a renderer that took only blocks drew every weather grey. Both
        /// orders are carried rather than one and a swizzle, because a `.dds` says which it is and
        /// the API has a format for each — converting would mean owning a copy of a buffer this
        /// type is defined by not owning.
        Rgba8Srgb,
        Bgra8Srgb,
    };

    /// How many bytes one block of a format occupies, or zero where its texels are not blocked.
    ///
    /// **Exhaustive rather than defaulted**, so that a format added to the enum is a build failure
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
                return 0;
        }

        throw Error("unknown texture format");
    }

    /// Whether a format's bytes are display-encoded, which every content format's are.
    ///
    /// The one that is not exists for tests: a value written into an `Rgba8Unorm` texture is the
    /// value light transport sees, with no transfer function between the expectation and the answer.
    inline bool isSrgb(TextureFormat format)
    {
        return format != TextureFormat::Rgba8Unorm;
    }

    /// A decoded texture, ready to upload and owning none of it.
    ///
    /// Deliberately not an `osg::Image`, and deliberately with no graphics API in it: every one of
    /// Morrowind's textures arrives block-compressed with its mip chain already built, so an upload
    /// is a copy and never a conversion — and it is the same copy whichever API performs it.
    struct TextureData
    {
        /// Which slot of the backend's array this is, which is the index a material holds.
        ///
        /// **Carried rather than implied by position.** Arrivals used to be a contiguous tail, so a
        /// backend could append and be right; a slot a departing cell freed is taken over wherever
        /// it sits, so an arrival has to say where it belongs.
        std::uint32_t mSlot = 0;

        TextureFormat mFormat = TextureFormat::Bc1RgbaSrgb;
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        /// Every level, back to back. The levels index into this.
        std::span<const std::byte> mBytes;
        std::span<const MipLevel> mLevels;

        /// The light already painted into it, as `SHADING_EXTENT` squared factors to divide out.
        ///
        /// Empty where nothing estimated one, which the shader reads as neutral. Described here
        /// rather than computed by the backend for the reason the texels are: what is true of
        /// Morrowind's content is worked out once and uploaded by whichever API is present.
        std::span<const float> mShading;

        /// What to call it in a capture — the file it came from. Spans storage the description's
        /// owner holds, like everything else here. Empty is allowed and only costs a nameless object
        /// in a debugger; every backend has somewhere to put it.
        std::string_view mName;
    };
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include <components/rtx/texturedata.hpp>

namespace Rtx::Testing
{
    /// One level over one texel, which is what most of the textures these tests build are.
    ///
    /// **A constant of static storage, because a description spans its levels rather than holding
    /// them.** A level written beside the call outlives nothing, so every test that wrote one had
    /// to keep it alive by hand and in the right scope.
    inline constexpr MipLevel sOneTexel{ 0, 1, 1 };

    /// A one-texel description over `texel`, whose four bytes the caller keeps.
    inline TextureData describeTexel(std::span<const std::uint8_t> texel, std::uint32_t slot = 0)
    {
        return TextureData{
            .mSlot = slot,
            .mFormat = TextureFormat::Rgba8Unorm,
            .mWidth = 1,
            .mHeight = 1,
            .mBytes = std::as_bytes(texel),
            .mLevels = std::span(&sOneTexel, 1),
        };
    }

    /// A texture a test paints by hand, and the storage its description spans.
    ///
    /// **Moved and never copied**, which the deleted copy holds it to: `mData` carries spans into
    /// the two vectors beside it, and a copy would hand back a description reading the original's
    /// buffers. A move keeps the vectors' buffers where they are, so the spans stay right and a
    /// painter can hand one back by value.
    struct TestTexture
    {
        std::vector<std::uint8_t> mBytes;
        std::vector<MipLevel> mLevels;
        TextureData mData;

        TestTexture() = default;
        TestTexture(TestTexture&&) noexcept = default;
        TestTexture& operator=(TestTexture&&) noexcept = default;
        TestTexture(const TestTexture&) = delete;
        TestTexture& operator=(const TestTexture&) = delete;

        /// Describes what a painter wrote, over the levels it pushed.
        ///
        /// @param format what those bytes are to be read as. Unorm by default, which is the one
        ///        with no transfer function under it and so the one a hand-computed byte belongs in.
        void describe(std::uint32_t width, std::uint32_t height, std::string_view name,
            TextureFormat format = TextureFormat::Rgba8Unorm)
        {
            mData = TextureData{
                .mFormat = format,
                .mWidth = width,
                .mHeight = height,
                .mBytes = std::as_bytes(std::span(mBytes)),
                .mLevels = mLevels,
                .mName = name,
            };
        }
    };

    /// A texture of exactly these texels at `extent` square: one level, uncompressed and not
    /// display-encoded, so what comes back out is what went in.
    ///
    /// The multi-texel counterpart of `describeTexel`, over the storage `TestTexture` holds for a
    /// description that spans rather than owns.
    inline void paintFlat(
        TestTexture& texture, std::uint32_t extent, std::span<const std::uint8_t> texels, std::string_view name)
    {
        texture.mBytes.assign(texels.begin(), texels.end());
        texture.mLevels.assign(1, MipLevel{ 0, extent, extent });
        texture.describe(extent, extent, name);
    }

    /// Adds one level of `width` by `height` to an uncompressed texture whose levels' alphas a
    /// test states outright, with colour it ignores, and describes it over every level so far.
    inline void addAlphaLevel(
        TestTexture& texture, std::uint32_t width, std::uint32_t height, std::initializer_list<std::uint8_t> alphas)
    {
        texture.mLevels.push_back(MipLevel{ static_cast<std::uint32_t>(texture.mBytes.size()), width, height });
        for (const std::uint8_t alpha : alphas)
        {
            texture.mBytes.insert(texture.mBytes.end(), 3, std::uint8_t{ 255 });
            texture.mBytes.push_back(alpha);
        }

        texture.describe(texture.mLevels.front().mWidth, texture.mLevels.front().mHeight, "alpha sheet");
    }

    /// How many texels across `paintTwoTones` paints.
    inline constexpr std::uint32_t sTwoTonesExtent = 128;

    /// A texture whose shading estimate is known: two tones along `u`, bytes of 255 across the
    /// columns `[from, to)` of a hundred and twenty-eight and 156 outside them — 1.0 and 0.3325 in
    /// light, a factor of three. Display-encoded unless `format` says otherwise, so a device
    /// decodes it as it decodes the game's. The estimate normalises to a mean of one, so with the
    /// bright half a half the bright cells come to 1.501 and the dark ones to the floor, 0.5, with
    /// the blur reaching three cells either side of a boundary. A hundred and twenty-eight across,
    /// so a cell is four texels, a block's width: the one size at which the host's per-block
    /// estimate and the device's per-texel one name the same cell for every texel.
    inline TestTexture paintTwoTones(
        std::uint32_t from, std::uint32_t to, TextureFormat format = TextureFormat::Rgba8Srgb)
    {
        constexpr std::uint32_t extent = sTwoTonesExtent;
        std::vector<std::uint8_t> texels(std::size_t{ extent } * extent * 4, 255);
        for (std::uint32_t y = 0; y < extent; ++y)
            for (std::uint32_t x = 0; x < extent; ++x)
                for (std::size_t channel = 0; channel < 3; ++channel)
                    texels[(std::size_t{ y } * extent + x) * 4 + channel] = x >= from && x < to ? 255 : 156;

        TestTexture painted;
        paintFlat(painted, extent, texels, "two tones");
        painted.describe(extent, extent, "two tones", format);
        return painted;
    }
}

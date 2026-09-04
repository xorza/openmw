#pragma once

#include <cstdint>
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
    /// **Painted in place and never copied**, which the deleted copy holds it to: `mData` carries
    /// spans into the two vectors beside it, and a copy would hand back a description reading the
    /// original's buffers.
    struct TestTexture
    {
        std::vector<std::uint8_t> mBytes;
        std::vector<MipLevel> mLevels;
        TextureData mData;

        TestTexture() = default;
        TestTexture(const TestTexture&) = delete;
        TestTexture& operator=(const TestTexture&) = delete;

        /// Describes what a painter wrote, over the levels it pushed.
        void describe(std::uint32_t width, std::uint32_t height, std::string_view name)
        {
            mData = TextureData{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = width,
                .mHeight = height,
                .mBytes = std::as_bytes(std::span(mBytes)),
                .mLevels = mLevels,
                .mName = name,
            };
        }
    };
}

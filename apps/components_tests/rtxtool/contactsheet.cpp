#include <array>
#include <cstddef>
#include <span>

#include <gtest/gtest.h>

#include <components/rtx/shaders/scene.h>
#include <components/rtx/texturedata.hpp>

#include <apps/rtxtool/contactsheet.hpp>

namespace RtxTool
{
    namespace
    {
        constexpr std::size_t sCells = std::size_t{ Rtx::Shaders::SHADING_EXTENT } * Rtx::Shaders::SHADING_EXTENT;

        /// The sheet has to show what the frame does, or it is a picture of a different renderer.
        ///
        /// **Block-compressed and not the linear test format, deliberately.** A content texture's
        /// bytes are display-encoded, so dividing in linear and encoding again lands on a byte the
        /// shader would also produce — which is what makes this a cross-check rather than two
        /// independent claims. The linear format would not: there the file holds the light itself,
        /// and the sheet's byte and the frame's byte are answers to different questions.
        ///
        /// One block, both endpoints `0x8410`, which is five-six-five `(16, 32, 16)` and replicates
        /// to `(132, 130, 132)`. Red is 132, or 0.517647, which is 0.230740 in linear; halved and
        /// encoded again, `1.055 * 0.115370^(1/2.4) - 0.055` is 95 of 255. The pixel test in the
        /// renderer works the same arithmetic for the same reason.
        TEST(RtxContactSheetTest, theSheetAppliesTheCorrectionTheFrameApplies)
        {
            // c0 and c1 equal, so every texel is that one colour whichever palette rule applies.
            constexpr std::array<std::uint8_t, 8> block{ 0x10, 0x84, 0x10, 0x84, 0x00, 0x00, 0x00, 0x00 };
            constexpr Rtx::MipLevel one{ 0, 4, 4 };
            const std::array<float, sCells> painted = [] {
                std::array<float, sCells> values{};
                values.fill(2.0f);
                return values;
            }();

            const Rtx::TextureData grey{
                .mFormat = Rtx::TextureFormat::Bc1RgbaSrgb,
                .mWidth = 4,
                .mHeight = 4,
                .mBytes = std::as_bytes(std::span(block)),
                .mLevels = std::span(&one, 1),
                .mShading = painted,
            };

            const auto shownAt = [&](float strength, bool right) {
                const ContactSheet sheet = drawContactSheet(std::span(&grey, 1), strength);
                EXPECT_EQ(sheet.mCount, 1u);

                const std::uint32_t x
                    = sheet.getLeftOf(0) + (right ? ContactSheet::getStride() : 0) + ContactSheet::getThumbnail() / 2;
                const std::uint32_t y = sheet.getTopOf(0) + ContactSheet::getThumbnail() / 2;
                return static_cast<int>(sheet.mPixels[(std::size_t{ y } * sheet.mWidth + x) * 4]);
            };

            EXPECT_EQ(shownAt(1.0f, false), 132) << "the left half is the texture as it was drawn";
            EXPECT_NEAR(shownAt(1.0f, true), 95, 1) << "and the right is it de-lit";

            // At no strength the two halves are the same picture, which is what makes the sheet an
            // A/B rather than a claim.
            EXPECT_EQ(shownAt(0.0f, true), 132);
        }

        /// A cell that used no textures has no sheet to draw, and says so rather than writing one.
        TEST(RtxContactSheetTest, nothingToShowDrawsNothing)
        {
            const ContactSheet sheet = drawContactSheet({}, 1.0f);

            EXPECT_EQ(sheet.mCount, 0u);
            EXPECT_TRUE(sheet.mPixels.empty());
        }
    }
}

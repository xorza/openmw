#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include <apps/rtxtool/instruments/contactsheet.hpp>
#include <components/rtx/texturedata.hpp>

#include "../rtx/testtexture.hpp"

namespace RtxTool
{
    namespace
    {
        /// The sheet has to show what the frame does, or it is a picture of a different renderer.
        ///
        /// **Display-encoded, deliberately.** A content texture's bytes are display-encoded, so
        /// dividing in linear and encoding again lands on a byte the shader would also produce —
        /// which is what makes this a cross-check rather than two independent claims. The linear
        /// test format would not: there the file holds the light itself, and the sheet's byte and
        /// the frame's byte are answers to different questions.
        ///
        /// `Rtx::Testing::paintTwoTones` says what the texture is: 255 across its middle half under an
        /// estimate of 1.501, which is 0.66624 in light and encodes to 213 of 255. The pixel test
        /// in the renderer, `aTexturesPaintedLightIsDividedBackOutOfItsAlbedo`, works the same
        /// arithmetic for the same reason.
        TEST(RtxContactSheetTest, theSheetAppliesTheCorrectionTheFrameApplies)
        {
            const Rtx::Testing::TestTexture painted = Rtx::Testing::paintTwoTones(32, 96);

            const auto shownAt = [&](float strength, bool right) {
                const ContactSheet sheet = drawContactSheet(std::span(&painted.mData, 1), strength);
                EXPECT_EQ(sheet.mCount, 1u);

                const std::uint32_t x
                    = sheet.getLeftOf(0) + (right ? ContactSheet::getStride() : 0) + ContactSheet::getThumbnail() / 2;
                const std::uint32_t y = sheet.getTopOf(0) + ContactSheet::getThumbnail() / 2;
                return static_cast<int>(sheet.mPixels[(std::size_t{ y } * sheet.mWidth + x) * 4]);
            };

            EXPECT_EQ(shownAt(1.0f, false), 255) << "the left half is the texture as it was drawn";
            EXPECT_NEAR(shownAt(1.0f, true), 213, 1) << "and the right is it de-lit";

            // At no strength the two halves are the same picture, which is what makes the sheet an
            // A/B rather than a claim.
            EXPECT_EQ(shownAt(0.0f, true), 255);
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

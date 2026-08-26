#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/renderer.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/guipass.hpp>
#include <components/rtxvulkan/image.hpp>
#include <components/rtxvulkan/instance.hpp>
#include <components/rtxvulkan/texture.hpp>
#include <components/rtxvulkan/validation.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sExtent = 8;

        /// What the target holds before anything is drawn over it, so that a blend has something to
        /// blend with and an untouched pixel is recognisable.
        constexpr std::array<std::uint8_t, 4> sBackground{ 0, 0, 255, 255 };

        constexpr std::array<std::uint8_t, 4> sWhiteTexel{ 255, 255, 255, 255 };

        /// A packed vertex colour, in the order MyGUI writes one: red in the low byte.
        constexpr std::uint32_t packColour(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha)
        {
            return static_cast<std::uint32_t>(red) | (static_cast<std::uint32_t>(green) << 8)
                | (static_cast<std::uint32_t>(blue) << 16) | (static_cast<std::uint32_t>(alpha) << 24);
        }

        /// Two triangles of a rectangle in clip space, with texture coordinates over the whole of it.
        ///
        /// Given MyGUI's orientation rather than Vulkan's: `top` is the coordinate nearer +1,
        /// because MyGUI computes its vertices for a clip space with +Y up.
        std::array<GuiVertex, 6> makeQuad(float left, float top, float right, float bottom, std::uint32_t colour)
        {
            const GuiVertex topLeft{ left, top, 0.0f, colour, 0.0f, 0.0f };
            const GuiVertex topRight{ right, top, 0.0f, colour, 1.0f, 0.0f };
            const GuiVertex bottomLeft{ left, bottom, 0.0f, colour, 0.0f, 1.0f };
            const GuiVertex bottomRight{ right, bottom, 0.0f, colour, 1.0f, 1.0f };

            return { topLeft, bottomLeft, bottomRight, topLeft, bottomRight, topRight };
        }

        /// A texture of exactly these texels: uncompressed and not display-encoded, so what comes
        /// back out is what went in.
        struct FlatTexture
        {
            std::vector<std::uint8_t> mBytes;
            std::array<MipLevel, 1> mLevels{};
            TextureData mData;

            FlatTexture(std::uint32_t extent, std::span<const std::uint8_t> texels)
                : mBytes(texels.begin(), texels.end())
            {
                mLevels[0] = MipLevel{ 0, extent, extent };
                mData = TextureData{
                    .mFormat = TextureFormat::Rgba8Unorm,
                    .mWidth = extent,
                    .mHeight = extent,
                    .mBytes = std::as_bytes(std::span(mBytes)),
                    .mLevels = mLevels,
                    .mName = "gui test texture",
                };
            }
        };

        class RtxGuiPassTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                std::string reason;
                mHarness = Testing::getHarness(reason);
                if (mHarness == nullptr)
                    GTEST_SKIP() << reason;

                mHarness->mInstance->getValidationLog()->clear();

                mPool = std::make_unique<CommandPool>(*mHarness->mDevice);
                mPass = std::make_unique<GuiPass>(
                    *mHarness->mDevice, Testing::getShaderDirectory(), VK_FORMAT_R8G8B8A8_UNORM);
            }

            void TearDown() override
            {
                if (mHarness == nullptr)
                    return;

                mPass.reset();
                mPool.reset();

                for (const ValidationMessage& message :
                    mHarness->mInstance->getValidationLog()->getErrorsOnThisThread())
                    ADD_FAILURE() << "validation error: " << message.mText;
            }

            /// Clears a target to `sBackground`, records `draws` over it, and hands back the pixels.
            /// Four bytes each, row zero at the top.
            void drawAndRead(
                std::span<const GuiVertex> vertices, std::span<const GuiDraw> draws, std::vector<std::uint8_t>& pixels)
            {
                Device& device = *mHarness->mDevice;

                Image target(device, sExtent, sExtent, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                        | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    "gui test target");

                Batch upload(*mPool);
                const Buffer buffer = uploadBuffer(device, upload, vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                upload.flush();

                mPool->submitAndWait([&](VkCommandBuffer commands) {
                    target.transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

                    const VkClearColorValue clear{ .float32 = { sBackground[0] / 255.0f, sBackground[1] / 255.0f,
                                                       sBackground[2] / 255.0f, sBackground[3] / 255.0f } };
                    const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    vkCmdClearColorImage(
                        commands, target.getHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &whole);

                    target.transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

                    mPass->record(commands, target, buffer.getHandle(), draws);
                });

                target.read(*mPool, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, pixels);
                ASSERT_EQ(pixels.size(), std::size_t{ sExtent } * sExtent * 4);
            }

            /// The four bytes at a pixel, row zero at the top.
            static std::array<std::uint8_t, 4> at(
                const std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y)
            {
                const std::size_t offset = (static_cast<std::size_t>(y) * sExtent + x) * 4;
                return { pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3] };
            }

            Testing::Harness* mHarness = nullptr;
            /// A texture on the device, waited for. The renderer records these into a batch it
            /// flushes once for a whole cell; a test wants the one texture ready on the next line.
            Texture makeTexture(const TextureData& data, std::string_view name)
            {
                Batch upload(*mPool);
                Texture texture(*mHarness->mDevice, upload, data, name);
                upload.flush();
                return texture;
            }

            std::unique_ptr<CommandPool> mPool;
            std::unique_ptr<GuiPass> mPass;
        };

        /// Half-transparent red over opaque blue, on the left half only.
        ///
        /// **Every expected byte is exact.** An alpha of 128/255 makes the source's contribution
        /// `255 * 128/255 = 128` and what it leaves of the destination `255 * 127/255 = 127`, so the
        /// blend has no rounding to argue about — and the right half says the draw stayed inside its
        /// own triangles.
        TEST_F(RtxGuiPassTest, aHalfTransparentQuadBlendsOverWhatWasAlreadyThere)
        {
            const FlatTexture white(1, sWhiteTexel);
            const Texture texture = makeTexture(white.mData, "white");

            const std::array<GuiVertex, 6> quad = makeQuad(-1.0f, 1.0f, 0.0f, -1.0f, packColour(255, 0, 0, 128));
            const std::array<GuiDraw, 1> draws{ GuiDraw{ texture.getView(), 0, quad.size() } };

            std::vector<std::uint8_t> pixels;
            ASSERT_NO_FATAL_FAILURE(drawAndRead(quad, draws, pixels));

            for (std::uint32_t y = 0; y < sExtent; ++y)
            {
                EXPECT_EQ(at(pixels, 1, y), (std::array<std::uint8_t, 4>{ 128, 0, 127, 255 })) << "covered, row " << y;
                EXPECT_EQ(at(pixels, sExtent - 2, y), sBackground) << "uncovered, row " << y;
            }
        }

        /// The texture is what the quad shows, and its first row is the one at the top of the frame.
        ///
        /// **This is the assertion that catches a flipped V.** MyGUI puts texture coordinate zero at
        /// the top of a widget and clip coordinate +1 there too; Vulkan's clip space points the other
        /// way, and the pass answers that with a flipped viewport rather than by touching the
        /// coordinates. Get it wrong and every glyph in the game is upside down — which is obvious on
        /// a screen and invisible to every other assertion here.
        TEST_F(RtxGuiPassTest, theTexturesFirstRowLandsAtTheTopOfTheFrame)
        {
            // Two by two, every corner distinguishable: red and green across the first row, blue and
            // white across the second.
            constexpr std::array<std::uint8_t, 16> sCorners{
                255,
                0,
                0,
                255, //
                0,
                255,
                0,
                255, //
                0,
                0,
                255,
                255, //
                255,
                255,
                255,
                255,
            };
            const FlatTexture corners(2, sCorners);
            const Texture texture = makeTexture(corners.mData, "corners");

            // The whole frame, opaque white so the texture passes through the multiply unchanged.
            const std::array<GuiVertex, 6> quad = makeQuad(-1.0f, 1.0f, 1.0f, -1.0f, packColour(255, 255, 255, 255));
            const std::array<GuiDraw, 1> draws{ GuiDraw{ texture.getView(), 0, quad.size() } };

            std::vector<std::uint8_t> pixels;
            ASSERT_NO_FATAL_FAILURE(drawAndRead(quad, draws, pixels));

            // A pixel's centre this far into a corner lands outside the outermost texel centre, so
            // clamping gives the filter one texel to return rather than a mixture of two.
            EXPECT_EQ(at(pixels, 1, 1), (std::array<std::uint8_t, 4>{ 255, 0, 0, 255 })) << "top left";
            EXPECT_EQ(at(pixels, sExtent - 2, 1), (std::array<std::uint8_t, 4>{ 0, 255, 0, 255 })) << "top right";
            EXPECT_EQ(at(pixels, 1, sExtent - 2), (std::array<std::uint8_t, 4>{ 0, 0, 255, 255 })) << "bottom left";
            EXPECT_EQ(at(pixels, sExtent - 2, sExtent - 2), (std::array<std::uint8_t, 4>{ 255, 255, 255, 255 }))
                << "bottom right";
        }

        /// Two batches, two textures, one buffer: the second must not be drawn with the first's.
        ///
        /// **What a per-batch push descriptor is for.** A GUI frame is dozens of these — a skin
        /// atlas, then a font, then another skin — and binding once for all of them is a mistake
        /// that looks like a font drawn in wood panelling.
        TEST_F(RtxGuiPassTest, eachBatchIsDrawnWithItsOwnTexture)
        {
            constexpr std::array<std::uint8_t, 4> sGreenTexel{ 0, 255, 0, 255 };
            const FlatTexture white(1, sWhiteTexel);
            const FlatTexture green(1, sGreenTexel);
            const Texture whiteTexture = makeTexture(white.mData, "white");
            const Texture greenTexture = makeTexture(green.mData, "green");

            const std::array<GuiVertex, 6> left = makeQuad(-1.0f, 1.0f, 0.0f, -1.0f, packColour(255, 0, 0, 255));
            const std::array<GuiVertex, 6> right = makeQuad(0.0f, 1.0f, 1.0f, -1.0f, packColour(255, 255, 255, 255));

            std::array<GuiVertex, 12> vertices{};
            std::copy(left.begin(), left.end(), vertices.begin());
            std::copy(right.begin(), right.end(), vertices.begin() + left.size());

            const std::array<GuiDraw, 2> draws{
                GuiDraw{ whiteTexture.getView(), 0, left.size() },
                GuiDraw{ greenTexture.getView(), left.size(), right.size() },
            };

            std::vector<std::uint8_t> pixels;
            ASSERT_NO_FATAL_FAILURE(drawAndRead(vertices, draws, pixels));

            // White texture times a red vertex colour on the left; green texture times white on the
            // right. Either texture used for both batches would make one of these the other.
            EXPECT_EQ(at(pixels, 1, 4), (std::array<std::uint8_t, 4>{ 255, 0, 0, 255 })) << "first batch";
            EXPECT_EQ(at(pixels, sExtent - 2, 4), (std::array<std::uint8_t, 4>{ 0, 255, 0, 255 })) << "second batch";
        }

        /// The same quad drawn additively and drawn over, side by side in one recording.
        ///
        /// **Both halves in one call on purpose**: it is the only assertion that the pass rebinds
        /// when the mode changes rather than drawing everything with whichever pipeline came first.
        ///
        /// Half-transparent red over opaque blue, so the arithmetic is exact either way. The source
        /// contributes `255 * 128/255 = 128` red in both. What separates them is the blue already
        /// there: `Over` keeps `255 * 127/255 = 127` of it, `Additive` keeps all 255. That gap is
        /// the whole difference between a hit flash reading as light and reading as a tint.
        TEST_F(RtxGuiPassTest, anAdditiveBatchAddsToTheFrameWhereAnOverOneReplacesIt)
        {
            const FlatTexture white(1, sWhiteTexel);
            const Texture texture = makeTexture(white.mData, "white");

            const std::array<GuiVertex, 6> left = makeQuad(-1.0f, 1.0f, 0.0f, -1.0f, packColour(255, 0, 0, 128));
            const std::array<GuiVertex, 6> right = makeQuad(0.0f, 1.0f, 1.0f, -1.0f, packColour(255, 0, 0, 128));

            std::array<GuiVertex, 12> vertices{};
            std::copy(left.begin(), left.end(), vertices.begin());
            std::copy(right.begin(), right.end(), vertices.begin() + left.size());

            const std::array<GuiDraw, 2> draws{
                GuiDraw{ texture.getView(), 0, left.size(), Blend::Additive },
                GuiDraw{ texture.getView(), left.size(), right.size(), Blend::Over },
            };

            std::vector<std::uint8_t> pixels;
            ASSERT_NO_FATAL_FAILURE(drawAndRead(vertices, draws, pixels));

            for (std::uint32_t y = 0; y < sExtent; ++y)
            {
                EXPECT_EQ(at(pixels, 1, y), (std::array<std::uint8_t, 4>{ 128, 0, 255, 255 })) << "additive, row " << y;
                EXPECT_EQ(at(pixels, sExtent - 2, y), (std::array<std::uint8_t, 4>{ 128, 0, 127, 255 }))
                    << "over, row " << y;
            }
        }

        /// Nothing to draw records nothing at all, rather than an empty render pass over the frame.
        TEST_F(RtxGuiPassTest, aFrameWithNoBatchesLeavesTheTargetAlone)
        {
            const std::array<GuiVertex, 6> quad = makeQuad(-1.0f, 1.0f, 1.0f, -1.0f, packColour(255, 0, 0, 255));

            std::vector<std::uint8_t> pixels;
            ASSERT_NO_FATAL_FAILURE(drawAndRead(quad, {}, pixels));

            for (std::uint32_t y = 0; y < sExtent; ++y)
                for (std::uint32_t x = 0; x < sExtent; ++x)
                    EXPECT_EQ(at(pixels, x, y), sBackground) << "at " << x << ", " << y;
        }
    }
}

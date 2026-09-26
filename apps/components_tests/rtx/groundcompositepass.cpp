#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec4f>
#include <vulkan/vulkan_core.h>

#include <components/rtx/colour.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/refusal.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/ground.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/texturedata.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/device.hpp>
#include <components/rtxvulkan/frameslots.hpp>
#include <components/rtxvulkan/groundcompositepass.hpp>
#include <components/rtxvulkan/handles.hpp>
#include <components/rtxvulkan/image.hpp>
#include <components/rtxvulkan/scenebuffers.hpp>
#include <components/rtxvulkan/texture.hpp>
#include <components/vfs/pathutil.hpp>

#include "support/device/harness.hpp"
#include "support/device/texturepasses.hpp"
#include "support/layers.hpp"
#include "support/testtexture.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sExtent = Shaders::GROUND_COMPOSITE_EXTENT;

        /// The first level of each image a bake wrote, four bytes a texel as the image stores them:
        /// the albedo display-encoded, the gloss as it is. Empty for an image it was not asked for.
        struct Baked
        {
            std::vector<std::uint8_t> mAlbedo;
            std::vector<std::uint8_t> mGloss;
        };

        struct RtxGroundCompositePassTest : Testing::DeviceTest
        {
            /// Bakes a chunk of two layers into the images `outputs` names, each stood as the array
            /// stands one, in one bake.
            ///
            /// The chunk: a solid red under the mip ladder, masked by a two-weight grid that ramps
            /// from all red at the first texel centre to all ladder at the second, with the ladder
            /// tiled `tiling` times across the chunk. Where `authored`, the ladder is authored, so
            /// its alpha — its grey — is a roughness; `standsIn` describes it as the stand-in.
            Baked bakeOf(float tiling, std::uint32_t outputs, bool authored, bool standsIn = false)
            {
                Device& device = getDevice();
                const Testing::TexturePassSet passes(device);
                const SetLayout layout = TextureArray::describeLayout(device);
                const GroundCompositePass pass(device, Testing::getShaderDirectory(), layout.get());

                constexpr std::array<std::uint8_t, 4> red{ 255, 0, 0, 255 };
                Testing::TestTexture ladder;
                Testing::paintMipLadder(ladder);
                std::array<TextureData, 2> textures{ Testing::describeTexel(red, 0), ladder.mData };
                textures[1].mSlot = 1;
                if (standsIn)
                    textures[1].mSource = TextureSource::StandIn;

                constexpr std::array<float, 2> firstMask{ 1.0f, 0.0f };
                constexpr std::array<float, 2> secondMask{ 0.0f, 1.0f };

                SceneDesc scene;
                scene.textures().add(VFS::Path::NormalizedView("red.dds"));
                scene.textures().add(VFS::Path::NormalizedView("ladder.dds"));
                std::array layers{
                    Testing::layerOf(0, scene.materials().addMask(firstMask), 2, 1),
                    Testing::layerOf(
                        1, scene.materials().addMask(secondMask), 2, 1, osg::Vec4f(tiling, tiling, 0.0f, 0.0f)),
                };
                if (authored)
                    layers[1].mFlags = Shaders::LAYER_AUTHORED;
                Material chunk;
                chunk.mKind = MaterialKind::Terrain;
                chunk.mFlatten = true;
                chunk.mLayers = scene.materials().addLayers(layers);
                const Index material = scene.addMaterial(chunk);

                Batch setup(getPool());
                const auto made = [&](std::uint32_t output, TextureFormat format) {
                    return (outputs & output) != 0
                        ? std::move(Texture::composite(device, setup, format, "ground composite test").value())
                        : Texture();
                };
                const Texture albedo = made(Shaders::GROUND_COMPOSITE_ALBEDO, TextureFormat::Rgba8Srgb);
                const Texture gloss = made(Shaders::GROUND_COMPOSITE_GLOSS, TextureFormat::Rgba8Unorm);
                TextureArray array(device, setup, layout, passes.mPasses, 2);
                std::vector<Refusal> refused;
                array.write(setup, textures, refused);
                array.sync(FrameSlot{});
                const SceneBuffers buffers(device, setup, scene, {}, 1);

                Shaders::GpuTables tables{};
                buffers.describeTables(FrameSlot{}, tables);
                pass.record(setup.getCommands(), array.getSet(FrameSlot{}),
                    albedo.isEmpty() ? nullptr : &albedo.getImage(), gloss.isEmpty() ? nullptr : &gloss.getImage(),
                    Shaders::GroundCompositeConstants{
                        .mMaterials = tables.mMaterials,
                        .mLayers = tables.mLayers,
                        .mMasks = tables.mMasks,
                        .mMaterial = material,
                        .mOutputs = outputs,
                        .mTexels = array.getTexelsAddress(FrameSlot{}),
                    });
                setup.flush();

                Baked baked;
                if (!albedo.isEmpty())
                    albedo.getImage().read(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, baked.mAlbedo);
                if (!gloss.isEmpty())
                    gloss.getImage().read(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, baked.mGloss);
                return baked;
            }

            /// The byte the bake owes texel `x` of a row, in `channel`, with the ladder read at
            /// level `grey`: the mask ramps at `u * 2 - 0.5` held to the unit, so the second layer
            /// weighs that and the first the rest, red under grey.
            static int expected(std::uint32_t x, std::size_t channel, std::uint8_t grey)
            {
                const float u = (float(x) + 0.5f) / float(sExtent);
                const float second = std::clamp(u * 2.0f - 0.5f, 0.0f, 1.0f);
                const float ladder = second * float(grey) / 255.0f;
                const float linear = channel == 0 ? (1.0f - second) + ladder : ladder;
                return static_cast<int>(std::lround(toEncoded(linear) * 255.0f));
            }

            /// The byte the gloss owes texel `x` of a row, in `channel`, with the ladder read at
            /// level `grey`: the share of the weight on the ladder, the only layer that reflects,
            /// and the roughness over both, the red a Lambert layer counted at one.
            static int expectedGloss(std::uint32_t x, std::size_t channel, std::uint8_t grey)
            {
                const float u = (float(x) + 0.5f) / float(sExtent);
                const float second = std::clamp(u * 2.0f - 0.5f, 0.0f, 1.0f);
                const float gloss[4]{ second, (1.0f - second) + second * float(grey) / 255.0f, 0.0f, 1.0f };
                return static_cast<int>(std::lround(gloss[channel] * 255.0f));
            }
        };

        /// The device's composite is the sum the host works out, texel for texel: the layers at
        /// the weights their masks name, the ladder at the level the footprint calls for.
        ///
        /// **The level is what tiling proves.** The ladder is sixty-four texels across, so tiled
        /// once it crosses the chunk in sixty-four, under the five hundred and twelve the
        /// composite spends: a footprint of one texel, level nought, grey 40. Tiled sixteen times
        /// it crosses in a thousand and twenty-four, a footprint of two: level one, grey 70. The
        /// two differ by a level, so a bake that read every layer at its finest, or at the wrong
        /// level, is caught in every texel the ladder shows in.
        ///
        /// **The gloss is the same sum**, of how much of the ground reflects and how rough, with the
        /// ladder authored: its share is the ramp, and its roughness the grey of the level the
        /// footprint calls for, so the gloss is held to the same level as the albedo. An authored
        /// ladder that stands in is a Lambert layer, as the trace reads it: nothing reflects, at a
        /// roughness of one, in every texel.
        ///
        /// Within a byte, because the device sums in its own float order and rounds once at the
        /// store where the host rounds once at the end.
        ///
        /// **And a chunk with both is one sum**: the pair baked together is, byte for byte, what
        /// each image's own bake writes.
        TEST_F(RtxGroundCompositePassTest, theCompositeAndItsGlossAreTheStackSummedAtTheLevelTheFootprintCallsFor)
        {
            constexpr std::uint32_t albedoOnly = Shaders::GROUND_COMPOSITE_ALBEDO;
            constexpr std::uint32_t glossOnly = Shaders::GROUND_COMPOSITE_GLOSS;
            const std::vector<std::uint8_t> once = bakeOf(1.0f, albedoOnly, false).mAlbedo;
            const std::vector<std::uint8_t> sixteen = bakeOf(16.0f, albedoOnly, false).mAlbedo;
            const std::vector<std::uint8_t> glossOnce = bakeOf(1.0f, glossOnly, true).mGloss;
            const std::vector<std::uint8_t> glossSixteen = bakeOf(16.0f, glossOnly, true).mGloss;
            const std::vector<std::uint8_t> glossStandingIn = bakeOf(1.0f, glossOnly, true, true).mGloss;

            const Baked pair = bakeOf(1.0f, albedoOnly | glossOnly, true);
            EXPECT_EQ(pair.mAlbedo, bakeOf(1.0f, albedoOnly, true).mAlbedo)
                << "the pair's albedo is not its own bake's";
            EXPECT_EQ(pair.mGloss, glossOnce) << "the pair's gloss is not its own bake's";
            ASSERT_EQ(once.size(), std::size_t{ sExtent } * sExtent * 4);
            ASSERT_EQ(sixteen.size(), once.size());
            ASSERT_EQ(glossOnce.size(), once.size());
            ASSERT_EQ(glossSixteen.size(), once.size());
            ASSERT_EQ(glossStandingIn.size(), once.size());

            // Every texel of the first row, and a stride of rows after it: the mask is one weight
            // tall, so every row is the first.
            for (std::uint32_t y = 0; y < sExtent; y += 37)
                for (std::uint32_t x = 0; x < sExtent; ++x)
                    for (std::size_t channel = 0; channel < 4; ++channel)
                    {
                        const std::size_t at = (std::size_t{ y } * sExtent + x) * 4 + channel;
                        const int wantOnce = channel == 3 ? 255 : expected(x, channel, 40);
                        const int wantSixteen = channel == 3 ? 255 : expected(x, channel, 70);
                        EXPECT_NEAR(int{ once[at] }, wantOnce, 1)
                            << "tiled once at " << x << ", " << y << " channel " << channel;
                        EXPECT_NEAR(int{ sixteen[at] }, wantSixteen, 1)
                            << "tiled sixteen times at " << x << ", " << y << " channel " << channel;
                        EXPECT_NEAR(int{ glossOnce[at] }, expectedGloss(x, channel, 40), 1)
                            << "gloss tiled once at " << x << ", " << y << " channel " << channel;
                        EXPECT_NEAR(int{ glossSixteen[at] }, expectedGloss(x, channel, 70), 1)
                            << "gloss tiled sixteen times at " << x << ", " << y << " channel " << channel;
                        EXPECT_EQ(int{ glossStandingIn[at] }, channel == 0 || channel == 2 ? 0 : 255)
                            << "gloss of a ladder that stands in at " << x << ", " << y << " channel " << channel;
                    }

            // And three texels the doc derives by hand, so the sweep is known to be over a ramp:
            // texel 0 is u = 0.00098, all red, 255 and 0; texel 511 is all ladder, grey 40 encoded
            // 1.055 * (40/255)^(1/2.4) - 0.055 = 0.43259, or 110 of 255; texel 255 is u = 0.49902,
            // 0.49805 of the ladder and 0.50195 of the red, red 0.50195 + 0.49805 * 0.15686 =
            // 0.58008 encoded 0.78581, or 200, and green 0.07812 encoded 0.30967, or 79.
            EXPECT_EQ(int{ once[0] }, 255);
            EXPECT_EQ(int{ once[1] }, 0);
            EXPECT_NEAR(int{ once[511 * 4] }, 110, 1);
            EXPECT_NEAR(int{ once[511 * 4 + 1] }, 110, 1);
            EXPECT_NEAR(int{ once[255 * 4] }, 200, 1);
            EXPECT_NEAR(int{ once[255 * 4 + 1] }, 79, 1);

            // The gloss at the same three: texel 0 reflects nothing at a roughness of one, 0 and
            // 255; texel 511 is all ladder, 255 and 40; texel 255 reflects 0.49805, or 127, at
            // 0.50195 + 0.49805 * 0.15686 = 0.58008, or 148.
            EXPECT_EQ(int{ glossOnce[0] }, 0);
            EXPECT_EQ(int{ glossOnce[1] }, 255);
            EXPECT_EQ(int{ glossOnce[511 * 4] }, 255);
            EXPECT_NEAR(int{ glossOnce[511 * 4 + 1] }, 40, 1);
            EXPECT_NEAR(int{ glossOnce[255 * 4] }, 127, 1);
            EXPECT_NEAR(int{ glossOnce[255 * 4 + 1] }, 148, 1);
        }
    }
}

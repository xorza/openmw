#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
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

#include "harness.hpp"
#include "layers.hpp"
#include "testtexture.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sExtent = Shaders::GROUND_COMPOSITE_EXTENT;

        struct RtxGroundCompositePassTest : Testing::DeviceTest
        {
            /// Bakes a chunk of two layers into a composite stood as the array stands one and
            /// hands its first level back, four bytes a texel as the image stores them,
            /// display-encoded.
            ///
            /// The chunk: a solid red under the mip ladder, masked by a two-weight grid that ramps
            /// from all red at the first texel centre to all ladder at the second, with the ladder
            /// tiled `tiling` times across the chunk.
            std::vector<std::uint8_t> bakeOf(float tiling)
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

                constexpr std::array<float, 2> firstMask{ 1.0f, 0.0f };
                constexpr std::array<float, 2> secondMask{ 0.0f, 1.0f };

                SceneDesc scene;
                scene.textures().add(VFS::Path::NormalizedView("red.dds"));
                scene.textures().add(VFS::Path::NormalizedView("ladder.dds"));
                const std::array layers{
                    Testing::layerOf(0, scene.materials().addMask(firstMask), 2, 1),
                    Testing::layerOf(
                        1, scene.materials().addMask(secondMask), 2, 1, osg::Vec4f(tiling, tiling, 0.0f, 0.0f)),
                };
                Material chunk;
                chunk.mKind = MaterialKind::Terrain;
                chunk.mFlatten = true;
                chunk.mLayers = scene.materials().addLayers(layers);
                const Index material = scene.addMaterial(chunk);

                Batch setup(getPool());
                const Texture composite = std::move(
                    Texture::composite(device, setup, TextureFormat::Rgba8Srgb, "ground composite test").value());
                TextureArray array(device, setup, layout, passes.mPasses, 2);
                std::vector<Refusal> refused;
                array.write(setup, textures, refused);
                array.sync(FrameSlot{});
                const SceneBuffers buffers(device, setup, scene, {}, 1);

                Shaders::GpuTables tables{};
                buffers.describeTables(FrameSlot{}, tables);
                pass.record(setup.getCommands(), array.getSet(FrameSlot{}), composite.getImage(),
                    Shaders::GroundCompositeConstants{
                        .mMaterials = tables.mMaterials,
                        .mLayers = tables.mLayers,
                        .mMasks = tables.mMasks,
                        .mMaterial = material,
                    });
                setup.flush();

                std::vector<std::uint8_t> read;
                composite.getImage().read(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, read);
                return read;
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
        /// Within a byte, because the device sums in its own float order and rounds once at the
        /// store where the host rounds once at the end.
        TEST_F(RtxGroundCompositePassTest, theCompositeIsTheStackSummedAtTheLevelTheFootprintCallsFor)
        {
            const std::vector<std::uint8_t> once = bakeOf(1.0f);
            const std::vector<std::uint8_t> sixteen = bakeOf(16.0f);
            ASSERT_EQ(once.size(), std::size_t{ sExtent } * sExtent * 4);
            ASSERT_EQ(sixteen.size(), once.size());

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
        }
    }
}

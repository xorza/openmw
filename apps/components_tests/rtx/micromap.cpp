#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/alphaimage.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/instancerecord.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/micromap.h>
#include <components/rtx/texturedata.hpp>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/graveyard.hpp>
#include <components/rtxvulkan/micromappass.hpp>
#include <components/rtxvulkan/sceneacceleration.hpp>
#include <components/rtxvulkan/scenebuffers.hpp>
#include <components/rtxvulkan/scenemicromaps.hpp>
#include <components/rtxvulkan/texture.hpp>

#include "geometry.hpp"
#include "harness.hpp"
#include "testtexture.hpp"

namespace Rtx
{
    namespace
    {
        using Shaders::Microtriangle;

        /// A microtriangle's cell: what `microtriangleIndex` is asked, read back off its corners.
        struct Cell
        {
            std::uint32_t mU;
            std::uint32_t mV;
            bool mFlipped;

            bool operator==(const Cell& other) const = default;
        };

        Cell cellOf(const Microtriangle& corners)
        {
            const std::uint32_t iu = std::min({ corners.mU0, corners.mU1, corners.mU2 });
            const std::uint32_t iv = std::min({ corners.mV0, corners.mV1, corners.mV2 });
            const std::uint32_t nearest
                = std::min({ corners.mU0 + corners.mV0, corners.mU1 + corners.mV1, corners.mU2 + corners.mV2 });

            return Cell{ iu, iv, nearest > iu + iv };
        }

        /// The curve and its inverse are one bijection over the cells, at every level the bake uses
        /// and the two under it.
        ///
        /// **Both directions and the cells between them.** The inverse is a descent and the forward
        /// is the specification's bit arithmetic, and nothing about either says they describe the
        /// same curve until every index has gone round the loop — and every cell has been named
        /// once, which is what says the descent lands on microtriangles and not on the same one
        /// twice.
        TEST(RtxMicromapCurveTest, theCurveAndItsInverseAreOneBijectionOverTheCells)
        {
            for (std::uint32_t level = 0; level <= Shaders::MICROMAP_LEVEL_MAX; ++level)
            {
                const std::uint32_t steps = 1u << level;
                const std::uint32_t count = Shaders::microtriangleCount(level);

                // One flag per cell, upright and flipped, over the whole square: the square holds
                // twice the triangle's cells, and the ones past the diagonal must stay unset.
                std::vector<std::uint8_t> named(std::size_t{ steps } * steps * 2, 0);

                for (std::uint32_t index = 0; index < count; ++index)
                {
                    const Microtriangle corners = Shaders::microtriangleAt(level, index);
                    const Cell cell = cellOf(corners);

                    ASSERT_LT(cell.mU + cell.mV + (cell.mFlipped ? 1u : 0u), steps)
                        << "level " << level << " index " << index << " lies outside the triangle";
                    ASSERT_EQ(Shaders::microtriangleIndex(level, cell.mU, cell.mV, cell.mFlipped), index)
                        << "level " << level;

                    // A unit microtriangle: its corners are the cell's own, so each coordinate is
                    // the cell's or one past it.
                    for (const std::uint32_t u : { corners.mU0, corners.mU1, corners.mU2 })
                        ASSERT_TRUE(u == cell.mU || u == cell.mU + 1) << "level " << level << " index " << index;
                    for (const std::uint32_t v : { corners.mV0, corners.mV1, corners.mV2 })
                        ASSERT_TRUE(v == cell.mV || v == cell.mV + 1) << "level " << level << " index " << index;

                    std::uint8_t& flag
                        = named[(std::size_t{ cell.mV } * steps + cell.mU) * 2 + (cell.mFlipped ? 1 : 0)];
                    ASSERT_EQ(flag, 0) << "level " << level << " names cell " << cell.mU << ", " << cell.mV
                                       << (cell.mFlipped ? " flipped" : "") << " twice";
                    flag = 1;
                }

                ASSERT_EQ(static_cast<std::uint32_t>(std::count(named.begin(), named.end(), 1)), count)
                    << "level " << level;
            }
        }

        /// The four children of a microtriangle at one level are the four indices at four times its
        /// own at the next, and they fill it.
        ///
        /// A child's corner is a parent's corner or the midpoint of two of them, doubled into the
        /// finer grid — which is the whole of what makes the curve hierarchical, and what lets a
        /// coarser level's answer stand for a finer one's.
        TEST(RtxMicromapCurveTest, aMicrotrianglesChildrenAreTheNextLevelsFourAtFourTimesItsIndex)
        {
            for (std::uint32_t level = 0; level < Shaders::MICROMAP_LEVEL_MAX; ++level)
                for (std::uint32_t index = 0; index < Shaders::microtriangleCount(level); ++index)
                {
                    const Microtriangle parent = Shaders::microtriangleAt(level, index);

                    // The parent's corners and midpoints in the children's grid, which is twice as
                    // fine: the six points a child may stand on.
                    const std::array<std::uint32_t, 3> pu{ parent.mU0, parent.mU1, parent.mU2 };
                    const std::array<std::uint32_t, 3> pv{ parent.mV0, parent.mV1, parent.mV2 };
                    std::array<std::array<std::uint32_t, 2>, 6> allowed{};
                    for (std::size_t at = 0; at < 3; ++at)
                    {
                        allowed[at] = { 2 * pu[at], 2 * pv[at] };
                        allowed[3 + at] = { pu[at] + pu[(at + 1) % 3], pv[at] + pv[(at + 1) % 3] };
                    }

                    std::array<Cell, 4> cells{};
                    for (std::uint32_t child = 0; child < 4; ++child)
                    {
                        const Microtriangle corners = Shaders::microtriangleAt(level + 1, 4 * index + child);
                        cells[child] = cellOf(corners);

                        for (const auto& [u, v] : { std::pair{ corners.mU0, corners.mV0 },
                                 std::pair{ corners.mU1, corners.mV1 }, std::pair{ corners.mU2, corners.mV2 } })
                            EXPECT_TRUE(std::find(allowed.begin(), allowed.end(), std::array{ u, v }) != allowed.end())
                                << "level " << level << " index " << index << " child " << child << " reaches " << u
                                << ", " << v;
                    }

                    for (std::size_t a = 0; a < 4; ++a)
                        for (std::size_t b = a + 1; b < 4; ++b)
                            EXPECT_FALSE(cells[a] == cells[b]) << "level " << level << " index " << index;
                }
        }

        /// The first two levels come out in the order the specification lists them.
        ///
        /// **Level one is the prose**: the corner at vertex nought, the middle, the corner at
        /// vertex one, the corner at vertex two. **Level two is the reference listing run by
        /// hand** over every cell, which is what pins the mirroring of the middle child and the
        /// last: a descent that visited them unmirrored would still be a bijection, and the data it
        /// baked would be read at every other microtriangle.
        TEST(RtxMicromapCurveTest, theFirstTwoLevelsAreOrderedAsTheSpecificationLists)
        {
            const std::array<Cell, 4> levelOne{ Cell{ 0, 0, false }, Cell{ 0, 0, true }, Cell{ 1, 0, false },
                Cell{ 0, 1, false } };
            for (std::uint32_t index = 0; index < levelOne.size(); ++index)
                EXPECT_EQ(cellOf(Shaders::microtriangleAt(1, index)), levelOne[index]) << "index " << index;

            const std::array<Cell, 16> levelTwo{
                Cell{ 0, 0, false },
                Cell{ 0, 0, true },
                Cell{ 1, 0, false },
                Cell{ 0, 1, false },
                Cell{ 0, 1, true },
                Cell{ 1, 1, false },
                Cell{ 1, 1, true },
                Cell{ 1, 0, true },
                Cell{ 2, 0, false },
                Cell{ 2, 0, true },
                Cell{ 3, 0, false },
                Cell{ 2, 1, false },
                Cell{ 1, 2, false },
                Cell{ 0, 2, true },
                Cell{ 0, 2, false },
                Cell{ 0, 3, false },
            };
            for (std::uint32_t index = 0; index < levelTwo.size(); ++index)
                EXPECT_EQ(cellOf(Shaders::microtriangleAt(2, index)), levelTwo[index]) << "index " << index;
        }

        /// A microtriangle promises what the least and the greatest of its texels allow, and the
        /// mean says which half an unknown folds to.
        TEST(RtxMicromapCurveTest, aStateIsConservativeInTheExtremesAndFoldsUnknownsByTheMean)
        {
            // least, most, sum, count, cutoff → state. Four texels apiece; the sums are the means
            // times four.
            EXPECT_EQ(Shaders::microtriangleState(0.6f, 1.0f, 3.2f, 4.0f, 0.5f), Shaders::MICROMAP_OPAQUE);
            EXPECT_EQ(Shaders::microtriangleState(0.5f, 0.5f, 2.0f, 4.0f, 0.5f), Shaders::MICROMAP_OPAQUE)
                << "at the cutoff is on the mask, as the trace's >= has it";
            EXPECT_EQ(Shaders::microtriangleState(0.0f, 0.4f, 1.0f, 4.0f, 0.5f), Shaders::MICROMAP_TRANSPARENT);
            EXPECT_EQ(Shaders::microtriangleState(0.0f, 1.0f, 3.0f, 4.0f, 0.5f), Shaders::MICROMAP_UNKNOWN_OPAQUE)
                << "three of four opaque";
            EXPECT_EQ(Shaders::microtriangleState(0.0f, 1.0f, 1.0f, 4.0f, 0.5f), Shaders::MICROMAP_UNKNOWN_TRANSPARENT)
                << "one of four opaque";
            EXPECT_EQ(Shaders::microtriangleState(0.0f, 1.0f, 2.0f, 4.0f, 0.5f), Shaders::MICROMAP_UNKNOWN_OPAQUE)
                << "a mean at the cutoff folds to opaque, as the extremes do";
            EXPECT_EQ(Shaders::microtriangleState(0.49f, 0.51f, 2.0f, 4.0f, 0.5f), Shaders::MICROMAP_UNKNOWN_OPAQUE)
                << "a texel just under the cutoff is what makes the microtriangle unknown";
        }

        /// The kernel's rule on the host, over `AlphaImage`: the same footprint, the same box of
        /// texels, the same arithmetic in the same order — so that the two can be compared word for
        /// word rather than state by state.
        ///
        /// **Written as the kernel is and not as a host would**, down to `float` throughout: what
        /// this proves is that the device decodes and decides as the host reads the same bytes, and
        /// a host oracle that rounded differently would report the device wrong at a texel edge.
        struct HostOracle
        {
            const AlphaImage& mAlpha;
            std::array<osg::Vec2f, 3> mCorner;
            float mCutoff;

            osg::Vec2f onSheet(std::uint32_t u, std::uint32_t v, float step) const
            {
                const float fu = static_cast<float>(u) * step;
                const float fv = static_cast<float>(v) * step;

                return mCorner[0] * (1.0f - fu - fv) + mCorner[1] * fu + mCorner[2] * fv;
            }

            std::uint32_t stateOf(const Microtriangle& micro, float step) const
            {
                osg::Vec2f q0 = onSheet(micro.mU0, micro.mV0, step);
                osg::Vec2f q1 = onSheet(micro.mU1, micro.mV1, step);
                osg::Vec2f q2 = onSheet(micro.mU2, micro.mV2, step);

                const osg::Vec2f base(
                    std::floor(std::min({ q0.x(), q1.x(), q2.x() })), std::floor(std::min({ q0.y(), q1.y(), q2.y() })));
                q0 -= base;
                q1 -= base;
                q2 -= base;

                for (const osg::Vec2f& q : { q0, q1, q2 })
                    if (std::isnan(q.x()) || std::isnan(q.y()))
                        return Shaders::MICROMAP_UNKNOWN_OPAQUE;

                for (osg::Vec2f* q : { &q0, &q1, &q2 })
                    *q = osg::Vec2f(std::min(q->x(), 2.0f), std::min(q->y(), 2.0f));

                const int width = static_cast<int>(mAlpha.getWidth());
                const int height = static_cast<int>(mAlpha.getHeight());
                const osg::Vec2f extent(static_cast<float>(width), static_cast<float>(height));

                const auto texelOf = [&](const osg::Vec2f& q) {
                    return osg::Vec2f(q.x() * extent.x() - 0.5f, q.y() * extent.y() - 0.5f);
                };
                const osg::Vec2f p0 = texelOf(q0);
                const osg::Vec2f p1 = texelOf(q1);
                const osg::Vec2f p2 = texelOf(q2);

                int firstX = static_cast<int>(std::floor(std::min({ p0.x(), p1.x(), p2.x() })));
                int firstY = static_cast<int>(std::floor(std::min({ p0.y(), p1.y(), p2.y() })));
                int lastX = static_cast<int>(std::floor(std::max({ p0.x(), p1.x(), p2.x() }))) + 1;
                int lastY = static_cast<int>(std::floor(std::max({ p0.y(), p1.y(), p2.y() }))) + 1;

                if (lastX - firstX + 1 >= width)
                {
                    firstX = 0;
                    lastX = width - 1;
                }
                if (lastY - firstY + 1 >= height)
                {
                    firstY = 0;
                    lastY = height - 1;
                }

                const auto wrapped
                    = [](int at, int size) { return at < 0 ? at + size : (at >= size ? at - size : at); };
                const auto alphaAt = [&](int x, int y) {
                    return static_cast<float>(mAlpha.at(0, static_cast<std::uint32_t>(wrapped(x, width)),
                               static_cast<std::uint32_t>(wrapped(y, height))))
                        / 255.0f;
                };

                const int spanX = lastX - firstX + 1;
                const int spanY = lastY - firstY + 1;

                if (static_cast<std::uint32_t>(spanX * spanY) > Shaders::MICROMAP_TEXEL_BUDGET)
                {
                    constexpr auto samples = static_cast<int>(Shaders::MICROMAP_SPARSE_SAMPLES);

                    float sampled = 0.0f;
                    for (int j = 0; j < samples; ++j)
                        for (int i = 0; i < samples; ++i)
                            sampled += alphaAt(firstX + (spanX * (2 * i + 1)) / (2 * samples),
                                firstY + (spanY * (2 * j + 1)) / (2 * samples));

                    return sampled >= mCutoff * static_cast<float>(samples * samples)
                        ? Shaders::MICROMAP_UNKNOWN_OPAQUE
                        : Shaders::MICROMAP_UNKNOWN_TRANSPARENT;
                }

                float least = 1.0f;
                float most = 0.0f;
                float sum = 0.0f;
                for (int y = firstY; y <= lastY; ++y)
                    for (int x = firstX; x <= lastX; ++x)
                    {
                        const float alpha = alphaAt(x, y);
                        least = std::min(least, alpha);
                        most = std::max(most, alpha);
                        sum += alpha;
                    }

                return Shaders::microtriangleState(least, most, sum, static_cast<float>(spanX * spanY), mCutoff);
            }

            /// One word of a triangle's data at `level`: sixteen states packed as the kernel packs
            /// them.
            std::uint32_t wordAt(std::uint32_t level, std::uint32_t word) const
            {
                const float step = 1.0f / static_cast<float>(1u << level);

                std::uint32_t packed = 0;
                for (std::uint32_t slot = 0; slot < Shaders::MICROMAP_STATES_PER_WORD; ++slot)
                {
                    const Microtriangle micro
                        = Shaders::microtriangleAt(level, word * Shaders::MICROMAP_STATES_PER_WORD + slot);
                    packed |= stateOf(micro, step) << (Shaders::MICROMAP_STATE_BITS * slot);
                }

                return packed;
            }
        };

        /// A mask sixteen texels square whose columns two to twelve are opaque and the rest
        /// transparent, with texel (5, 0) one step under the cutoff. `theKernelDecides...` says what
        /// each microtriangle of a triangle over it comes to.
        void paintMask(Testing::TestTexture& texture)
        {
            constexpr std::uint32_t extent = 16;

            texture.mLevels.push_back(MipLevel{ 0, extent, extent });
            texture.mBytes.assign(std::size_t{ extent } * extent * 4, 0);

            for (std::uint32_t y = 0; y < extent; ++y)
                for (std::uint32_t x = 0; x < extent; ++x)
                {
                    std::uint8_t* const texel = &texture.mBytes[(std::size_t{ y } * extent + x) * 4];
                    texel[0] = 255;
                    texel[3] = x >= 2 && x <= 12 ? 255 : 0;
                }

            texture.mBytes[(std::size_t{ 0 } * extent + 5) * 4 + 3] = 127;

            texture.describe(extent, extent, "mask");
        }

        /// The mask laid over `Testing::sUnitTriangle`, corner for corner.
        const std::array<osg::Vec2f, 3> sTriangleUv{
            osg::Vec2f(0.0f, 0.0f),
            osg::Vec2f(1.0f, 0.0f),
            osg::Vec2f(0.0f, 1.0f),
        };
        /// The same triangle with the mask repeated a hundred times along it, so every microtriangle
        /// at level two covers twenty-five repeats and is past the budget.
        const std::array<osg::Vec2f, 3> sRepeatedUv{
            osg::Vec2f(0.0f, 0.0f),
            osg::Vec2f(100.0f, 0.0f),
            osg::Vec2f(0.0f, 100.0f),
        };

        struct RtxMicromapBakeTest : Testing::DeviceTest
        {
        };

        /// Bakes one triangle of `uv` over the mask at `level` through the kernel and reads its
        /// first word back, beside the oracle's word for the same triangle.
        struct BakedWord
        {
            std::uint32_t mKernel = 0;
            std::uint32_t mOracle = 0;
        };

        BakedWord bakeOne(Device& device, CommandPool& pool, const Testing::TestTexture& mask,
            std::span<const osg::Vec2f, 3> uv, std::uint32_t level)
        {
            SceneDesc scene;
            const Index cutout = scene.addMaterial(Material{
                .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("mask.dds")),
                .mAlphaRef = 0.5f,
                .mAlphaMode = AlphaMode::Cutout,
            });
            const Index triangle = scene.addMesh(
                Testing::sUnitTriangle, {}, uv, Testing::sTriangleIndices, {}, Deform::None, sNoIndex, cutout);

            Graveyard graveyard(device, pool);
            Batch setup(pool);
            const SceneAcceleration acceleration(device, scene, 1);
            const SceneBuffers buffers(device, scene, {}, 1, graveyard);
            const TextureArray textures(device, setup, 1, std::span(&mask.mData, 1), graveyard);
            const MicromapPass pass(device, textures.getLayout(), Testing::getShaderDirectory());

            // The kernel's own inputs and output, owned here so the word can be read back: one
            // `VkMicromapTriangleEXT` at `level`, and the first word of data for it.
            constexpr VkBufferUsageFlags addressable = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            const Buffer triangles = Buffer::hostWritten(device, sizeof(VkMicromapTriangleEXT), addressable);
            const VkMicromapTriangleEXT described{
                .dataOffset = 0,
                .subdivisionLevel = static_cast<std::uint16_t>(level),
                .format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT,
            };
            triangles.write(std::span<const VkMicromapTriangleEXT>(&described, 1));

            const VkDeviceSize bytes = Shaders::microtriangleWords(level) * sizeof(std::uint32_t);
            const Buffer data = Buffer::deviceLocal(device, bytes, addressable | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            const Buffer read = Buffer::staging(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

            const MeshRange& mesh = scene.getMeshes()[triangle];
            const Material& material = scene.getMaterials()[cutout];

            const VkCommandBuffer commands = setup.getCommands();
            pass.begin(commands, textures.getSet());
            pass.bake(commands,
                Shaders::MicromapConstants{
                    .mIndices = acceleration.getIndices(mesh),
                    .mTexCoords = buffers.getTexCoords(mesh),
                    .mTriangles = triangles.getDeviceAddress(),
                    .mData = data.getDeviceAddress(),
                    .mTransform = material.mTextureTransform,
                    .mTexture = material.mDiffuse,
                    .mCount = 1,
                    .mCutoff = material.getAlphaCutoff(),
                    .mPadding = 0,
                });

            const VkMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            };
            const VkDependencyInfo dependency{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = &barrier,
            };
            vkCmdPipelineBarrier2(commands, &dependency);

            const VkBufferCopy whole{ .size = bytes };
            vkCmdCopyBuffer(commands, data.getHandle(), read.getHandle(), 1, &whole);
            setup.flush();

            BakedWord word;
            std::memcpy(&word.mKernel, read.map(), sizeof(word.mKernel));

            const AlphaImage alpha(mask.mData);
            const HostOracle oracle{
                .mAlpha = alpha,
                .mCorner = { uv[0], uv[1], uv[2] },
                .mCutoff = material.getAlphaCutoff(),
            };
            word.mOracle = oracle.wordAt(level, 0);

            return word;
        }

        /// The kernel decides each microtriangle as the host oracle does, word for word, and the
        /// word is the one worked out by hand.
        ///
        /// **The states, by cell.** At level two a microtriangle is a quarter of the sheet across,
        /// four texels of sixteen, and its bilinear support runs a texel past either side: the
        /// column of cells at `u` in `[k/4, (k+1)/4]` reads columns `4k - 1` to `4k + 4`. Columns
        /// two to twelve are opaque, so cell column one reads three to eight and column two seven
        /// to twelve — **opaque**, both. Cell column nought reads fifteen and nought to four, three
        /// opaque of six: a mean exactly at the cutoff, **unknown opaque**. Cell column three
        /// reads eleven to fifteen and nought, two opaque of six, **unknown transparent**. And the
        /// odd texel at (5, 0), one step under the cutoff, is read by the cells at column one
        /// whose rows reach row nought — `v` in `[0, 1/4]`, which reads rows fifteen and nought to
        /// four — so `(1, 0)` upright and flipped are **unknown opaque**, thirty-five texels opaque
        /// and one not. The triangle's rows are otherwise alike, so a cell's state is its column's.
        TEST_F(RtxMicromapBakeTest, theKernelDecidesEachMicrotriangleAsTheHostOracleDoes)
        {
            constexpr std::uint32_t level = 2;

            Testing::TestTexture mask;
            paintMask(mask);
            const BakedWord word = bakeOne(getDevice(), getPool(), mask, sTriangleUv, level);
            const std::uint32_t baked = word.mKernel;

            // By hand first, cell by cell, as the doc comment works them out.
            const auto stateAt = [&](std::uint32_t iu, std::uint32_t iv, bool flipped) {
                const std::uint32_t index = Shaders::microtriangleIndex(level, iu, iv, flipped);
                return (baked >> (Shaders::MICROMAP_STATE_BITS * index)) & 0x3u;
            };

            for (std::uint32_t iu = 0; iu < 4; ++iu)
                for (std::uint32_t iv = 0; iu + iv < 4; ++iv)
                    for (const bool flipped : { false, true })
                    {
                        if (flipped && iu + iv >= 3)
                            continue;

                        std::uint32_t expected = Shaders::MICROMAP_OPAQUE;
                        if (iu == 0)
                            expected = Shaders::MICROMAP_UNKNOWN_OPAQUE;
                        else if (iu == 3)
                            expected = Shaders::MICROMAP_UNKNOWN_TRANSPARENT;
                        else if (iu == 1 && iv == 0)
                            expected = Shaders::MICROMAP_UNKNOWN_OPAQUE;

                        EXPECT_EQ(stateAt(iu, iv, flipped), expected)
                            << "cell " << iu << ", " << iv << (flipped ? " flipped" : "");
                    }

            // And the oracle, word for word, over the same bytes decoded on the host.
            EXPECT_EQ(baked, word.mOracle);
        }

        /// A microtriangle whose support is past the budget is unknown, folded by a sparse mean,
        /// and the kernel and the oracle agree about which half.
        ///
        /// **The sixteen samples, by hand.** Every microtriangle covers twenty-five repeats, so its
        /// support is clamped to the whole sixteen-texel mask on both axes and the four samples
        /// along each fall at the centres of four-texel bins: columns two, six, ten and fourteen,
        /// and the same rows. Columns two, six and ten are opaque and fourteen is not, and the odd
        /// texel at (5, 0) is on no sampled column — so twelve of sixteen samples are opaque, a mean
        /// of three quarters, and every microtriangle is **unknown opaque**.
        TEST_F(RtxMicromapBakeTest, aMicrotrianglePastTheBudgetIsUnknownByASparseMean)
        {
            constexpr std::uint32_t level = 2;

            Testing::TestTexture mask;
            paintMask(mask);
            const BakedWord word = bakeOne(getDevice(), getPool(), mask, sRepeatedUv, level);

            for (std::uint32_t index = 0; index < Shaders::microtriangleCount(level); ++index)
                EXPECT_EQ(
                    (word.mKernel >> (Shaders::MICROMAP_STATE_BITS * index)) & 0x3u, Shaders::MICROMAP_UNKNOWN_OPAQUE)
                    << "index " << index;

            EXPECT_EQ(word.mKernel, word.mOracle);
        }

        /// A cutout mesh arriving takes a micromap and a room, a mesh leaving gives both back
        /// through the graveyard, and a mesh worn by an animated material — or by a mask nothing has
        /// uploaded — takes none.
        ///
        /// **The room's return is what the storage size says.** A block is never shrunk, so the
        /// figure a second bake leaves it at is the assertion: a room given back is taken again and
        /// the storage stays what it was, where one never given back would make the storage grow.
        TEST_F(RtxMicromapBakeTest, aCutoutMeshTakesAMicromapAndGivesItBackAndAnAnimatedOneTakesNone)
        {
            Device& device = getDevice();
            CommandPool& pool = getPool();

            Testing::TestTexture mask;
            paintMask(mask);

            SceneDesc scene;
            const Index cutout = scene.addMaterial(Material{
                .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("mask.dds")),
                .mAlphaRef = 0.5f,
                .mAlphaMode = AlphaMode::Cutout,
            });
            Material scrolling = scene.getMaterials()[cutout];
            scrolling.mAnimated = true;
            const Index animated = scene.addMaterial(scrolling);

            // A mask the scene names and nothing describes: a slot the array does not hold.
            Material unopened = scene.getMaterials()[cutout];
            unopened.mDiffuse = scene.addTexture(VFS::Path::NormalizedView("nowhere.dds"));
            const Index untextured = scene.addMaterial(unopened);

            const Index baked = scene.addMesh(
                Testing::sUnitTriangle, {}, sTriangleUv, Testing::sTriangleIndices, {}, Deform::None, sNoIndex, cutout);
            const Index refused = scene.addMesh(Testing::sUnitTriangle, {}, sTriangleUv, Testing::sTriangleIndices, {},
                Deform::None, sNoIndex, animated);
            const Index bare = scene.addMesh(Testing::sUnitTriangle, {}, sTriangleUv, Testing::sTriangleIndices, {},
                Deform::None, sNoIndex, untextured);
            const Index plain = scene.addMesh(Testing::sUnitTriangle, {}, sTriangleUv, Testing::sTriangleIndices);

            for (const Index mesh : { baked, refused, bare, plain })
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = mesh,
                    .mMaterial = scene.getMeshes()[mesh].mMaterial });

            std::vector<InstanceRecord> records;
            makeInstanceRecords(scene, records);

            Graveyard graveyard(device, pool);
            Batch setup(pool);
            SceneAcceleration acceleration(device, scene, 1);
            const SceneBuffers buffers(device, scene, records, 1, graveyard);
            const TextureArray textures(device, setup, 1, std::span(&mask.mData, 1), graveyard);
            const MicromapPass pass(device, textures.getLayout(), Testing::getShaderDirectory());
            SceneMicromaps micromaps(device);

            micromaps.bake(
                setup, pass, scene, buffers, acceleration, textures, acceleration.getEveryMesh(), nullptr, graveyard);

            EXPECT_TRUE(micromaps.has(baked));
            EXPECT_FALSE(micromaps.has(refused)) << "a mask a controller scrolls cannot be baked";
            EXPECT_FALSE(micromaps.has(bare)) << "a mask nothing uploaded cannot be read";
            EXPECT_FALSE(micromaps.has(plain)) << "a mesh with no material has no mask";
            EXPECT_EQ(micromaps.getUntexturedCount(), 1u);

            const VkAccelerationStructureTrianglesOpacityMicromapEXT described = micromaps.describe(baked);
            EXPECT_EQ(described.indexType, VK_INDEX_TYPE_NONE_KHR) << "every triangle owns the entry at its own index";
            EXPECT_NE(described.micromap, VK_NULL_HANDLE);
            ASSERT_EQ(described.usageCountsCount, 1u) << "one triangle is one level";
            EXPECT_EQ(described.pUsageCounts[0].count, 1u);
            EXPECT_EQ(
                described.pUsageCounts[0].format, static_cast<std::uint32_t>(VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT));

            // The whole sheet over a triangle of half of it is 128 texels, and `log4(128 / 4)` is
            // two and a half — rounded away from the floor, so three.
            EXPECT_EQ(described.pUsageCounts[0].subdivisionLevel, 3u);

            // And a structure built over it, which is what the layers check the description
            // against: the counts must match the triangles and the micromap must be built first.
            acceleration.build(setup, scene, records, micromaps, graveyard);
            setup.flush();

            const VkDeviceSize held = micromaps.getBytes();
            EXPECT_GT(held, 0u);
            EXPECT_EQ(acceleration.getMicromappedInstanceCount(), 1u);
            EXPECT_EQ(acceleration.getCutoutInstanceCount(), 3u) << "the cutouts nothing could bake still stop rays";

            // Given back through the graveyard, and only once the graveyard says so.
            const std::array<Index, 1> going{ baked };
            micromaps.release(going, graveyard);
            EXPECT_FALSE(micromaps.has(baked));
            micromaps.release(going, graveyard);

            // Nothing is in flight: the batch was waited for. The graveyard destroys the micromap
            // and gives its room back, and a second bake of the same mesh takes that room again.
            device.waitIdle();
            acceleration.release(going, graveyard);
            graveyard.clear();

            Batch again(pool);
            micromaps.bake(again, pass, scene, buffers, acceleration, textures, going, nullptr, graveyard);
            acceleration.buildArrived(again, scene, micromaps, nullptr, graveyard);
            again.flush();

            EXPECT_TRUE(micromaps.has(baked));
            EXPECT_EQ(micromaps.getBytes(), held) << "the room given back was not the room taken again";

            device.waitIdle();
            acceleration.release(going, graveyard);
            micromaps.release(going, graveyard);
            graveyard.clear();
        }

        /// A material rewritten under the micromap baked against it is refused, and one rewritten
        /// in a way the bake cannot see is not.
        TEST_F(RtxMicromapBakeTest, aMaterialRewrittenUnderItsMicromapIsRefusedByName)
        {
            Device& device = getDevice();
            CommandPool& pool = getPool();

            Testing::TestTexture mask;
            paintMask(mask);

            SceneDesc scene;
            const Index cutout = scene.addMaterial(Material{
                .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("mask.dds")),
                .mAlphaRef = 0.5f,
                .mAlphaMode = AlphaMode::Cutout,
            });
            const Index baked = scene.addMesh(
                Testing::sUnitTriangle, {}, sTriangleUv, Testing::sTriangleIndices, {}, Deform::None, sNoIndex, cutout);

            Graveyard graveyard(device, pool);
            Batch setup(pool);
            const SceneAcceleration acceleration(device, scene, 1);
            const SceneBuffers buffers(device, scene, {}, 1, graveyard);
            const TextureArray textures(device, setup, 1, std::span(&mask.mData, 1), graveyard);
            const MicromapPass pass(device, textures.getLayout(), Testing::getShaderDirectory());
            SceneMicromaps micromaps(device);

            micromaps.bake(
                setup, pass, scene, buffers, acceleration, textures, acceleration.getEveryMesh(), nullptr, graveyard);
            setup.flush();
            ASSERT_TRUE(micromaps.has(baked));

            // The arrival's own writes: the material was added this frame, and it is what was baked.
            EXPECT_NO_THROW(micromaps.check(scene));
            scene.clearArrivals();

            // A rewrite the bake cannot see — the glow — is not a rewrite of the mask.
            Material glowing = scene.getMaterials()[cutout];
            glowing.mEmissiveColour = osg::Vec3f(1.0f, 0.0f, 0.0f);
            scene.setMaterial(cutout, glowing);
            EXPECT_NO_THROW(micromaps.check(scene));
            scene.clearArrivals();

            // A rewrite of the cutoff is.
            Material tightened = glowing;
            tightened.mAlphaRef = 0.75f;
            scene.setMaterial(cutout, tightened);
            EXPECT_THROW(micromaps.check(scene), Error);

            device.waitIdle();
        }
    }
}

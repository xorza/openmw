#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Array>
#include <osg/GL>
#include <osg/Image>
#include <osg/Node>
#include <osg/PrimitiveSet>
#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/esm/refid.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadland.hpp>
#include <components/rtx/contentsource.hpp>
#include <components/rtx/groundreader.hpp>
#include <components/rtx/preparedground.hpp>
#include <components/terrain/buffercache.hpp>
#include <components/vfs/pathutil.hpp>

#include "fakeland.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// Images and no templates, which is all the ground asks for.
        class GroundImages final : public ContentSource
        {
        public:
            osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView) override { return nullptr; }
            osg::ref_ptr<const osg::Image> getImage(const VFS::Path::NormalizedView path) override
            {
                return mImages.get(path);
            }

            ImagesByPath mImages;
        };

        /// One triangle list against the other, index for index.
        void expectSameTriangles(const std::span<const std::uint32_t> read, const osg::DrawElements& built)
        {
            ASSERT_EQ(read.size(), built.getNumIndices());
            for (std::size_t at = 0; at < read.size(); ++at)
                ASSERT_EQ(read[at], built.index(at)) << "index " << at;
        }

        /// A cell with a land record is the storage's own grid, cut the way `Terrain::BufferCache`
        /// cuts a chunk with no neighbour at another level, with the layers and the transforms the
        /// chunk manager's passes would have carried.
        TEST(RtxGroundReaderTest, aCellIsTheGamesOwnGridCutTheGamesOwnWay)
        {
            FakeLand land;
            land.mWithData = { osg::Vec2i(0, 0) };
            GroundImages images;
            GroundReader reader(land, images, ESM::Cell::sDefaultWorldspaceId);

            PreparedGround ground;
            reader.read(osg::Vec2i(0, 0), ground);

            EXPECT_TRUE(ground.mStands);
            EXPECT_EQ(ground.mOrigin, osg::Vec3f(0.5f * FakeLand::sCellSize, 0.5f * FakeLand::sCellSize, 0.0f));

            constexpr std::size_t verts = FakeLand::sVerts;
            ASSERT_EQ(ground.mPositions.size(), verts * verts);
            ASSERT_EQ(ground.mNormals.size(), verts * verts);
            ASSERT_EQ(ground.mTexCoords.size(), verts * verts);

            // Column 64, row 0: the eastern edge at the southern corner, by hand.
            EXPECT_EQ(ground.mPositions[64 * verts], osg::Vec3f(4096.0f, -4096.0f, 8.0f * 64));
            EXPECT_EQ(ground.mPositions[64 * verts], FakeLand::positionAt(64, 0));

            // **The game's own index buffer and the game's own corners**, from the class that
            // builds them for the rasterizer's chunks.
            Terrain::BufferCache buffers;
            expectSameTriangles(ground.mIndices, *buffers.getIndexBuffer(verts, 0));
            EXPECT_EQ(ground.mIndices.size(), (verts - 1) * (verts - 1) * 6);

            const osg::ref_ptr<osg::Vec2Array> corners = buffers.getUVBuffer(verts);
            ASSERT_EQ(corners->size(), ground.mTexCoords.size());
            for (std::size_t at = 0; at < corners->size(); ++at)
                ASSERT_EQ((*corners)[at], ground.mTexCoords[at]) << "corner " << at;

            // Two layers, each tiling sixteen times across the cell, each with the western or the
            // eastern half of a 34 × 34 mask.
            ASSERT_EQ(ground.mLayers.size(), 2u);
            const PreparedLayer& grass = ground.mLayers[0];
            const PreparedLayer& rock = ground.mLayers[1];
            EXPECT_EQ(grass.mImage->getFileName(), "textures/grass.dds");
            EXPECT_EQ(rock.mImage->getFileName(), "textures/rock.dds");
            EXPECT_EQ(grass.mDiffuseTransform, osg::Vec4f(16.0f, 16.0f, 0.0f, 0.0f));

            // `BlendmapTexMat` at sixteen tiles: a scale of 16 / 17 about the centre and a nudge of
            // a quarter texel, which comes to an offset of 0.75 / 17 in x and 0.25 / 17 in y.
            const osg::Vec4f mask = GroundReader::maskTransform(16);
            EXPECT_NEAR(mask.x(), 16.0f / 17.0f, 1e-6f);
            EXPECT_NEAR(mask.y(), 16.0f / 17.0f, 1e-6f);
            EXPECT_NEAR(mask.z(), 0.75f / 17.0f, 1e-6f);
            EXPECT_NEAR(mask.w(), 0.25f / 17.0f, 1e-6f);
            EXPECT_EQ(grass.mMaskTransform, mask);
            EXPECT_EQ(rock.mMaskTransform, mask);

            constexpr std::uint32_t side = FakeLand::sMaskSide;
            EXPECT_EQ(grass.mMaskWidth, side);
            EXPECT_EQ(grass.mMaskHeight, side);
            EXPECT_EQ(grass.mWeights.mCount, side * side);
            EXPECT_EQ(rock.mWeights.mCount, side * side);
            EXPECT_EQ(rock.mWeights.mOffset, side * side);
            ASSERT_EQ(ground.mWeights.size(), 2u * side * side);

            // Row 0: grass in the first column, rock in the last, by the byte's own reciprocal.
            EXPECT_EQ(ground.mWeights[grass.mWeights.mOffset], 1.0f);
            EXPECT_EQ(ground.mWeights[grass.mWeights.mOffset + side - 1], 0.0f);
            EXPECT_EQ(ground.mWeights[rock.mWeights.mOffset], 0.0f);
            EXPECT_EQ(ground.mWeights[rock.mWeights.mOffset + side - 1], 1.0f);
        }

        /// A cell with no land record is a plane at the default height on four corners, wearing
        /// the default ground.
        TEST(RtxGroundReaderTest, aCellWithNoRecordIsAPlaneAtTheDefaultHeight)
        {
            FakeLand land;
            GroundImages images;
            GroundReader reader(land, images, ESM::Cell::sDefaultWorldspaceId);

            PreparedGround ground;
            reader.read(osg::Vec2i(1, 0), ground);

            EXPECT_TRUE(ground.mStands);
            EXPECT_EQ(ground.mOrigin, osg::Vec3f(1.5f * FakeLand::sCellSize, 0.5f * FakeLand::sCellSize, 0.0f));

            ASSERT_EQ(ground.mPositions.size(), 4u);
            for (const osg::Vec3f& corner : ground.mPositions)
            {
                EXPECT_EQ(corner.z(), static_cast<float>(ESM::Land::DEFAULT_HEIGHT));
                EXPECT_EQ(std::abs(corner.x()), 0.5f * FakeLand::sCellSize);
                EXPECT_EQ(std::abs(corner.y()), 0.5f * FakeLand::sCellSize);
            }

            Terrain::BufferCache buffers;
            expectSameTriangles(ground.mIndices, *buffers.getIndexBuffer(2, 0));

            ASSERT_EQ(ground.mLayers.size(), 1u);
            EXPECT_EQ(ground.mLayers[0].mImage->getFileName(), "textures/_land_default.dds");
            EXPECT_EQ(ground.mLayers[0].mWeights.mCount, 0u) << "one ground type covers the cell";
            EXPECT_TRUE(ground.mWeights.empty());
        }

        /// A land whose one cell blends two masks of two formats: the game's own, and one a mod
        /// might ship.
        class TwoFormats final : public FakeLand
        {
        public:
            TwoFormats()
            {
                mWithData = { osg::Vec2i(0, 0) };

                // Sixteen by sixteen, so the game's format carries every byte a texel can hold —
                // and the other one carries them backwards, which is a different picture rather
                // than the same one twice.
                mAlpha->allocateImage(16, 16, 1, GL_ALPHA, GL_UNSIGNED_BYTE);
                mRgba->allocateImage(16, 16, 1, GL_RGBA, GL_UNSIGNED_BYTE);

                for (int row = 0; row < 16; ++row)
                {
                    unsigned char* alphaRow = mAlpha->data(0, row);
                    unsigned char* rgbaRow = mRgba->data(0, row);

                    for (int column = 0; column < 16; ++column)
                    {
                        const auto value = static_cast<unsigned char>(row * 16 + column);
                        alphaRow[column] = value;

                        rgbaRow[column * 4 + 0] = 7;
                        rgbaRow[column * 4 + 1] = 11;
                        rgbaRow[column * 4 + 2] = 13;
                        rgbaRow[column * 4 + 3] = static_cast<unsigned char>(255 - value);
                    }
                }
            }

            void getBlendmaps(float, const osg::Vec2f&, ImageVector& blendmaps, std::vector<Terrain::LayerInfo>& layers,
                ESM::RefId) override
            {
                layers.push_back(Terrain::LayerInfo{ VFS::Path::Normalized("ground0.dds"), {}, false, false });
                layers.push_back(Terrain::LayerInfo{ VFS::Path::Normalized("ground1.dds"), {}, false, false });
                blendmaps.push_back(mAlpha);
                blendmaps.push_back(mRgba);
            }

            osg::ref_ptr<osg::Image> mAlpha = new osg::Image;
            osg::ref_ptr<osg::Image> mRgba = new osg::Image;
        };

        /// A blend map is read exactly as `osg::Image::getColor` reads it, on both paths that read
        /// one.
        ///
        /// **The game's own masks are one byte a texel in `GL_ALPHA`**, and that one format is read
        /// along the row rather than a texel at a time: `getColor` decides on the pixel format and
        /// the data type per texel and builds a `Vec4` to hand back one component of it. Every
        /// other format still takes `getColor`, so a mod's blend map is read as it always was.
        ///
        /// **The two have to agree to the bit.** A weight is what a cell's ground is blended by and
        /// what its composite is baked from, and `getColor` multiplies by a reciprocal where a
        /// divide differs in the last place for 126 of the 256 byte values.
        TEST(RtxGroundReaderTest, aBlendMapReadsTheSameOnTheRowPathAndTheFallback)
        {
            TwoFormats land;
            GroundImages images;
            GroundReader reader(land, images, ESM::Cell::sDefaultWorldspaceId);

            PreparedGround ground;
            reader.read(osg::Vec2i(0, 0), ground);
            ASSERT_EQ(ground.mLayers.size(), 2u);

            const auto readsAs = [&](const PreparedLayer& layer, const osg::Image& image) {
                ASSERT_EQ(layer.mMaskWidth, 16u);
                ASSERT_EQ(layer.mMaskHeight, 16u);
                ASSERT_EQ(layer.mWeights.mCount, 256u);

                const std::span<const float> weights = layer.mWeights.in(std::span<const float>(ground.mWeights));
                for (int row = 0; row < 16; ++row)
                    for (int column = 0; column < 16; ++column)
                        ASSERT_EQ(weights[static_cast<std::size_t>(row) * 16 + column], image.getColor(column, row).a())
                            << "texel " << column << ", " << row << " of " << image.getPixelFormat();
            };

            readsAs(ground.mLayers[0], *land.mAlpha);
            readsAs(ground.mLayers[1], *land.mRgba);

            // And the two ends of the range by hand, which is the one claim `getColor` cannot be
            // asked to make about itself: an empty texel is no weight and a full one is all of it.
            EXPECT_EQ(ground.mWeights[ground.mLayers[0].mWeights.mOffset], 0.0f);
            EXPECT_EQ(ground.mWeights[ground.mLayers[0].mWeights.mOffset + 255], 1.0f);
        }
    }
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <osg/GL>
#include <osg/Image>
#include <osg/Texture>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/resource/imagemanager.hpp>
#include <components/rtx/compositequeue.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/refusals.hpp>
#include <components/rtx/result.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/spritelight.hpp>
#include <components/rtx/texturebuilder.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/textureencoding.hpp>
#include <components/rtx/texturewrap.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "allocations.hpp"
#include "geometry.hpp"
#include "heldimages.hpp"

namespace Rtx
{
    namespace
    {
        /// One block's worth of image in `format`, which is all the description reads beyond it.
        osg::ref_ptr<osg::Image> makeBlock(GLenum format)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setFileName("textures/tx_test.dds");
            image->allocateImage(4, 4, 1, format, GL_UNSIGNED_BYTE);
            return image;
        }

        /// Where a texture held to a side begins, and what it costs from there: its first level no
        /// larger than the side along either axis, and the bytes from that level to the end.
        ///
        /// **Hand-computed.** Sixteen by eight at four bytes a texel is 512 bytes, then eight by
        /// four at 128 and four by two at 32: 672 from the top, 160 from the second level and 32
        /// from the third. The width is what a side is held against, being the longer axis.
        TEST(RtxTextureBuilderTest, aTextureHeldToASideBeginsAtItsFirstLevelWithinIt)
        {
            constexpr std::array levels{ MipLevel{ 0, 16, 8 }, MipLevel{ 512, 8, 4 }, MipLevel{ 640, 4, 2 } };
            const std::array<std::byte, 672> bytes{};
            const TextureData texture{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = 16,
                .mHeight = 8,
                .mBytes = bytes,
                .mLevels = levels,
            };

            struct Case
            {
                std::uint32_t mSide;
                std::optional<std::uint32_t> mFirst;
            };
            for (const Case& one : { Case{ 16, 0 }, Case{ 15, 1 }, Case{ 8, 1 }, Case{ 7, 2 }, Case{ 4, 2 },
                     Case{ 3, std::nullopt }, Case{ 0, std::nullopt } })
                EXPECT_EQ(texture.firstLevelWithin(one.mSide), one.mFirst) << "held to " << one.mSide;

            EXPECT_EQ(texture.bytesFrom(0), 672u);
            EXPECT_EQ(texture.bytesFrom(1), 160u);
            EXPECT_EQ(texture.bytesFrom(2), 32u);

            // The stand-in is one block of four, and a device that took less has nothing to begin at.
            const TextureData standIn = describeStandIn();
            EXPECT_EQ(standIn.mSource, TextureSource::StandIn);
            EXPECT_EQ(standIn.firstLevelWithin(4), 0u);
            EXPECT_EQ(standIn.firstLevelWithin(3), std::nullopt);
            EXPECT_EQ(standIn.bytesFrom(0), 8u) << "one BC1 block";
        }

        /// DXT1 arrives under two names and both of them read the alpha bit.
        ///
        /// Almost none of Morrowind's DDS files set `DDPF_ALPHAPIXELS`, so OSG hands over its
        /// foliage as `GL_COMPRESSED_RGB_S3TC_DXT1_EXT`; taking that at its word decodes a canopy's
        /// punch-through blocks as opaque black and leaves every tree the card it was painted on.
        /// The bytes are identical either way — the format only decides whether the bit is looked at.
        TEST(RtxTextureBuilderTest, bothSpellingsOfDxt1ReadTheAlphaBit)
        {
            std::vector<Rtx::MipLevel> levels;
            std::vector<std::byte> texels;

            EXPECT_EQ(describeImage(*makeBlock(GL_COMPRESSED_RGB_S3TC_DXT1_EXT), levels, texels).value().mFormat,
                Rtx::TextureFormat::Bc1RgbaSrgb);
            EXPECT_EQ(describeImage(*makeBlock(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT), levels, texels).value().mFormat,
                Rtx::TextureFormat::Bc1RgbaSrgb);
        }

        /// The formats Morrowind actually ships, kept apart. DXT3 carrying its own alpha is what
        /// the handful of soft-edged masks in the game are stored as.
        TEST(RtxTextureBuilderTest, theOtherBlockFormatsKeepTheirOwnMapping)
        {
            std::vector<Rtx::MipLevel> levels;
            std::vector<std::byte> texels;

            EXPECT_EQ(describeImage(*makeBlock(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT), levels, texels).value().mFormat,
                Rtx::TextureFormat::Bc2Srgb);
            EXPECT_EQ(describeImage(*makeBlock(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT), levels, texels).value().mFormat,
                Rtx::TextureFormat::Bc3Srgb);
        }

        /// **A companion map is described as data: its blocks without the curve, and no painted light
        /// taken out of it**, because a normal map is no picture of anything lit. A two-channel file is
        /// taken as data and refused as a colour, where it has lost its blue.
        TEST(RtxTextureBuilderTest, dataIsDescribedWithoutTheCurveAndWithNeutralShading)
        {
            std::vector<Rtx::MipLevel> levels;
            std::vector<std::byte> texels;

            const Rtx::TextureData normal = describeImage(
                *makeBlock(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT), levels, texels, Rtx::TextureEncoding::Data)
                                                .value();
            EXPECT_EQ(normal.mFormat, Rtx::TextureFormat::Bc3Unorm);
            EXPECT_EQ(normal.mEncoding, Rtx::TextureEncoding::Data);
            EXPECT_TRUE(normal.hasNeutralShading());

            const Rtx::TextureData colour
                = describeImage(*makeBlock(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT), levels, texels).value();
            EXPECT_EQ(colour.mEncoding, Rtx::TextureEncoding::Colour);
            EXPECT_FALSE(colour.hasNeutralShading()) << "a colour file's painted light is estimated";

            EXPECT_EQ(
                describeImage(*makeBlock(GL_COMPRESSED_RED_GREEN_RGTC2_EXT), levels, texels, Rtx::TextureEncoding::Data)
                    .value()
                    .mFormat,
                Rtx::TextureFormat::Bc5Unorm);
            EXPECT_FALSE(describeImage(*makeBlock(GL_COMPRESSED_RED_GREEN_RGTC2_EXT), levels, texels).isOk())
                << "two channels are no colour";
        }

        /// Two images into one level table, each description still naming only its own.
        ///
        /// The description **appends** where it used to clear, which is what lets a whole scene's
        /// levels live in one buffer instead of one vector per texture — and what would let a second
        /// image quietly take over the first's span if the offset were ever taken wrong.
        TEST(RtxTextureBuilderTest, describingIntoOneTableLeavesEachImageItsOwnLevels)
        {
            const osg::ref_ptr<osg::Image> first = makeBlock(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT);
            const osg::ref_ptr<osg::Image> second = makeBlock(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);

            // Reserved up front for the same reason `SceneTextures` does it: the spans below point
            // into this, so it must not reallocate between the two calls.
            std::vector<Rtx::MipLevel> levels;
            std::vector<std::byte> texels;
            levels.reserve(first->getNumMipmapLevels() + second->getNumMipmapLevels());

            const Rtx::TextureData a = describeImage(*first, levels, texels).value();
            const Rtx::TextureData b = describeImage(*second, levels, texels).value();

            // A 4x4 block allocated without a chain is one level, so the table holds exactly two and
            // the second description begins where the first ends.
            ASSERT_EQ(levels.size(), std::size_t{ 2 });
            EXPECT_EQ(a.mLevels.size(), std::size_t{ 1 });
            EXPECT_EQ(b.mLevels.size(), std::size_t{ 1 });
            EXPECT_EQ(a.mLevels.data(), levels.data());
            EXPECT_EQ(b.mLevels.data(), levels.data() + 1);

            EXPECT_EQ(a.mFormat, Rtx::TextureFormat::Bc2Srgb);
            EXPECT_EQ(b.mFormat, Rtx::TextureFormat::Bc3Srgb);

            // Carried so a capture can name the object; a backend has nothing else to call it.
            EXPECT_EQ(a.mName, "textures/tx_test.dds");
        }

        /// The uncompressed spellings a `.dds` uses are taken, and both channel orders are kept.
        ///
        /// **Morrowind's meshes are block-compressed to the last file and its sky is not.** The
        /// cloud decks are 32-bit `DDPF_RGB`, so a renderer that took only blocks drew every
        /// weather's clouds as the unreadable stand-in — an opaque grey, which over a deck is the
        /// whole sky. The two orders are separate formats rather than one and a swizzle: a `.dds`
        /// says which it holds and the API has a format for each, where converting would mean owning
        /// a copy of a buffer `TextureData` is defined by not owning.
        TEST(RtxTextureBuilderTest, theUncompressedSpellingsAreTakenAndKeepTheirChannelOrder)
        {
            std::vector<Rtx::MipLevel> levels;
            std::vector<std::byte> texels;

            EXPECT_EQ(
                describeImage(*makeBlock(GL_RGBA), levels, texels).value().mFormat, Rtx::TextureFormat::Rgba8Srgb);
            EXPECT_EQ(
                describeImage(*makeBlock(GL_BGRA), levels, texels).value().mFormat, Rtx::TextureFormat::Bgra8Srgb);

            // **Display-encoded, which every content format is.** The one uncompressed format that
            // is not exists for tests asserting an exact texel, and a cloud texture read through it
            // would come out with its transfer function applied twice.
            EXPECT_TRUE(Rtx::isSrgb(Rtx::TextureFormat::Rgba8Srgb));
            EXPECT_TRUE(Rtx::isSrgb(Rtx::TextureFormat::Bgra8Srgb));
        }

        /// A format this still cannot upload fails by name rather than uploading noise, and so does
        /// an image of no size, which no device takes.
        ///
        /// Three-channel spellings are refused deliberately: uploading one would need the fourth
        /// channel written in, which means owning a buffer, and nothing this game ships stores an
        /// opaque texture without one.
        TEST(RtxTextureBuilderTest, aFormatWithNoAlphaChannelOrAnImageOfNoSizeIsRefusedAndSaysWhich)
        {
            std::vector<Rtx::MipLevel> levels;
            std::vector<std::byte> texels;
            const Result<Rtx::TextureData, std::string> rgb = describeImage(*makeBlock(GL_RGB), levels, texels);
            ASSERT_FALSE(rgb.isOk());
            EXPECT_EQ(rgb.error(), "its format is RGB8 (6407), which this renderer does not upload");

            // A format that uploads, so what is refused is the size alone.
            osg::ref_ptr<osg::Image> empty = new osg::Image;
            empty->setFileName("textures/tx_empty.dds");
            empty->setImage(0, 4, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, osg::Image::NO_DELETE);
            const Result<Rtx::TextureData, std::string> unsized = describeImage(*empty, levels, texels);
            ASSERT_FALSE(unsized.isOk()) << "an image of no size was described";
            EXPECT_EQ(unsized.error(), "it is 0 by 4 texels, which no device holds");
            EXPECT_TRUE(levels.empty()) << "a refusal adds no level";
        }

        /// A header counting levels past the single texel is cut at it, because a device takes no
        /// image with more levels than its size has. A 4x4 has three — 4, 2 and 1 across — and in
        /// RGBA they are 64, 16 and 4 bytes, at offsets 0, 64 and 80. The file counts two more.
        TEST(RtxTextureBuilderTest, aHeaderCountingLevelsPastOneTexelIsCutAtIt)
        {
            constexpr std::size_t sBytes = 64 + 16 + 4 + 4 + 4;

            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setFileName("textures/tx_deep.dds");
            image->setImage(
                4, 4, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, new unsigned char[sBytes], osg::Image::USE_NEW_DELETE);
            image->setMipmapLevels(osg::Image::MipmapDataType{ 64, 80, 84, 88 });
            ASSERT_EQ(image->getNumMipmapLevels(), 5u) << "the header's count, which is what this is about";

            std::vector<Rtx::MipLevel> levels;
            std::vector<std::byte> texels;
            const Rtx::TextureData described = describeImage(*image, levels, texels).value();

            ASSERT_EQ(described.mLevels.size(), 3u);
            EXPECT_EQ(described.mLevels[0].mOffset, 0u);
            EXPECT_EQ(described.mLevels[1].mOffset, 64u);
            EXPECT_EQ(described.mLevels[2].mOffset, 80u);
            EXPECT_EQ(described.mLevels[1].mWidth, 2u);
            EXPECT_EQ(described.mLevels[2].mWidth, 1u);
            EXPECT_EQ(described.mLevels[2].mHeight, 1u);
        }

        /// A two-by-two sixteen-bit image with its one-texel level, the five words little-endian —
        /// the two levels' offsets are nought and eight bytes.
        osg::ref_ptr<osg::Image> makeSixteenBit(GLenum type, GLint internal, const std::array<std::uint16_t, 5>& words)
        {
            auto* bytes = new unsigned char[words.size() * 2];
            for (std::size_t at = 0; at < words.size(); ++at)
            {
                bytes[at * 2] = static_cast<unsigned char>(words[at] & 0xFF);
                bytes[at * 2 + 1] = static_cast<unsigned char>(words[at] >> 8);
            }

            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setFileName("textures/tx_sixteen.dds");
            image->setImage(2, 2, 1, internal, type == GL_UNSIGNED_SHORT_5_6_5 ? GL_RGB : GL_BGRA, type, bytes,
                osg::Image::USE_NEW_DELETE);
            image->setMipmapLevels(osg::Image::MipmapDataType{ 8 });
            return image;
        }

        /// A sixteen-bit file is widened to RGBA8 in the encoding its slot asks for, every channel
        /// exactly: the byte nearest the channel's value over its top, so nought stays nought and a
        /// channel's top is 255.
        ///
        /// Hand-computed. `0x8410` is R5G6B5's 16, 32 and 16: `16 × 255 / 31 = 131.6` is 132 and
        /// `32 × 255 / 63 = 129.5` is 130. `0x1960` is 3, 11 and nought, where the nearest bytes
        /// part from the bits repeated: `3 × 255 / 31 = 24.7` is 25 and not 24, and
        /// `11 × 255 / 63 = 44.5` is 45 and not 44. Four bits of `v` are `17 v` either way.
        /// A1R5G5B5 keeps its alpha bit where X1R5G5B5 is opaque whatever the bit says, and the
        /// same for the four-bit pair. The levels begin at twice their offsets, nought and 16.
        TEST(RtxTextureBuilderTest, aSixteenBitFileIsWidenedToRgba8ExactlyInItsSlotsEncoding)
        {
            struct Case
            {
                GLenum mType;
                GLint mInternal;
                std::array<std::uint16_t, 5> mWords;
                std::array<std::uint8_t, 20> mTexels;
            };
            const std::array<Case, 5> cases{ {
                { GL_UNSIGNED_SHORT_5_6_5, GL_RGB, { 0xF800, 0x07E0, 0x001F, 0x8410, 0x1960 },
                    { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 132, 130, 132, 255, 25, 45, 0, 255 } },
                { GL_UNSIGNED_SHORT_1_5_5_5_REV, GL_RGBA, { 0x8000, 0x7C00, 0x03E0, 0x001F, 0xFFFF },
                    { 0, 0, 0, 255, 255, 0, 0, 0, 0, 255, 0, 0, 0, 0, 255, 0, 255, 255, 255, 255 } },
                { GL_UNSIGNED_SHORT_1_5_5_5_REV, GL_RGB, { 0x8000, 0x7C00, 0x03E0, 0x001F, 0x7FFF },
                    { 0, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 } },
                { GL_UNSIGNED_SHORT_4_4_4_4_REV, GL_RGBA, { 0xF000, 0x0F00, 0x00F0, 0x0008, 0x1234 },
                    { 0, 0, 0, 255, 255, 0, 0, 0, 0, 255, 0, 0, 0, 0, 136, 0, 34, 51, 68, 17 } },
                { GL_UNSIGNED_SHORT_4_4_4_4_REV, GL_RGB, { 0xF000, 0x0F00, 0x00F0, 0x0008, 0x1234 },
                    { 0, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 136, 255, 34, 51, 68, 255 } },
            } };

            for (const Case& one : cases)
            {
                const osg::ref_ptr<osg::Image> image = makeSixteenBit(one.mType, one.mInternal, one.mWords);
                std::vector<Rtx::MipLevel> levels;
                std::vector<std::byte> texels;

                const Rtx::TextureData described = describeImage(*image, levels, texels).value();
                EXPECT_EQ(described.mFormat, Rtx::TextureFormat::Rgba8Srgb) << "type " << one.mType;
                ASSERT_EQ(described.mBytes.size(), one.mTexels.size()) << "type " << one.mType;
                for (std::size_t at = 0; at < one.mTexels.size(); ++at)
                    EXPECT_EQ(std::to_integer<std::uint32_t>(described.mBytes[at]), one.mTexels[at])
                        << "type " << one.mType << ", byte " << at;

                ASSERT_EQ(described.mLevels.size(), 2u);
                EXPECT_EQ(described.mLevels[1].mOffset, 16u);
                EXPECT_EQ(described.mLevels[1].mWidth, 1u);
            }

            // As data, the same texels read linearly.
            const osg::ref_ptr<osg::Image> image = makeSixteenBit(GL_UNSIGNED_SHORT_5_6_5, GL_RGB, cases[0].mWords);
            std::vector<Rtx::MipLevel> levels;
            std::vector<std::byte> texels;
            EXPECT_EQ(describeImage(*image, levels, texels, Rtx::TextureEncoding::Data).value().mFormat,
                Rtx::TextureFormat::Rgba8Unorm);
        }

        /// An image whose levels its format and OpenSceneGraph count differently is refused by
        /// name before a byte of it is read, and one they agree on is described at its format's
        /// size.
        ///
        /// Three A1R5G5B5 texels a row are six bytes, and packed to four they are eight: the reader
        /// would walk the second row two bytes early. A second level stated at byte 20 of a
        /// two-by-two RGBA8 is four past where the first level ends. And a two-by-two BC3 with no
        /// chain is its one sixteen-byte block, which `getTotalSizeInBytes` counts as four.
        TEST(RtxTextureBuilderTest, aLevelTheFormatAndTheLoaderCountDifferentlyIsRefusedByName)
        {
            std::vector<Rtx::MipLevel> levels;
            std::vector<std::byte> texels;

            osg::ref_ptr<osg::Image> padded = new osg::Image;
            padded->setFileName("textures/tx_padded.dds");
            padded->setImage(3, 1, 1, GL_RGBA, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, new unsigned char[8],
                osg::Image::USE_NEW_DELETE, 4);
            const Result<Rtx::TextureData, std::string> rows = describeImage(*padded, levels, texels);
            ASSERT_FALSE(rows.isOk());
            EXPECT_EQ(rows.error(), "its level 0 is 8 bytes at byte 0, where A1R5G5B5 at 3 by 1 is 6 at byte 0");

            padded->setPacking(1);
            EXPECT_TRUE(describeImage(*padded, levels, texels).isOk()) << "the packing made no difference";
            levels.clear();

            osg::ref_ptr<osg::Image> shifted = new osg::Image;
            shifted->setFileName("textures/tx_shifted.dds");
            shifted->setImage(
                2, 2, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, new unsigned char[24], osg::Image::USE_NEW_DELETE);
            shifted->setMipmapLevels(osg::Image::MipmapDataType{ 20 });
            const Result<Rtx::TextureData, std::string> offset = describeImage(*shifted, levels, texels);
            ASSERT_FALSE(offset.isOk());
            EXPECT_EQ(offset.error(), "its level 1 is 4 bytes at byte 20, where RGBA8 at 1 by 1 is 4 at byte 16");
            EXPECT_TRUE(levels.empty()) << "a refusal adds no level";

            shifted->setMipmapLevels(osg::Image::MipmapDataType{ 16 });
            EXPECT_EQ(describeImage(*shifted, levels, texels).value().mBytes.size(), 20u);

            osg::ref_ptr<osg::Image> block = new osg::Image;
            block->allocateImage(2, 2, 1, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_UNSIGNED_BYTE);
            ASSERT_EQ(block->getTotalSizeInBytesIncludingMipmaps(), 4u) << "what the old span staged";
            EXPECT_EQ(describeImage(*block, levels, texels).value().mBytes.size(), 16u);
        }

        /// Describing an arrival a second time reaches the heap not at all.
        ///
        /// **What every scratch in `SceneTextures` is for, stated as a number.** A cell arriving is
        /// the frame with the least room to grow anything, and the class holds the images, the
        /// levels, the descriptions, the kept slots and the shading maps across arrivals so that one
        /// refills what the last one grew. A `clear()` traded for a fresh vector anywhere in there
        /// is what this catches, and nothing else would.
        ///
        /// **The first call is the one that grows them**, so it is spent and not measured — the same
        /// shape the frame-path guards use.
        TEST(RtxTextureBuilderTest, describingAnArrivalASecondTimeReachesTheHeapNotAtAll)
        {
            VFS::Manager vfs;
            Testing::HeldImages images(&vfs, 0);

            // **Held under the name the image carries**, so the slot, the cache key and the name
            // the description comes back with are one path rather than three.
            constexpr VFS::Path::NormalizedView path("textures/tx_test.dds");
            const osg::ref_ptr<osg::Image> image = makeBlock(GL_RGBA);
            ASSERT_EQ(image->getFileName(), path.value()) << "the slot and the image name a different file";
            images.hold(path, image);

            // And one widened, whose texels the class holds as it holds the levels.
            constexpr VFS::Path::NormalizedView sixteen("textures/tx_sixteen.dds");
            images.hold(sixteen, makeSixteenBit(GL_UNSIGNED_SHORT_5_6_5, GL_RGB, { 0, 0, 0, 0, 0 }));

            Rtx::SceneDesc scene;
            Testing::addModel(scene, path);
            Testing::addModel(scene, sixteen);

            SceneTextures described;
            described.describeAll(scene, images);
            ASSERT_TRUE(described.getRefusals().empty()) << "an image did not come back from the cache";
            ASSERT_EQ(described.getDescriptions().size(), std::size_t{ 2 });
            ASSERT_EQ(described.getDescriptions()[1].mBytes.size(), 20u) << "the widened texels were not described";

            // Four texels across and one level in the file, which is the level described: the rest
            // are the device's to make.
            ASSERT_EQ(described.getDescriptions()[0].mLevels.size(), std::size_t{ 1 });

            const std::size_t before = Testing::getAllocationCount();
            described.describeAll(scene, images);
            const std::size_t spent = Testing::getAllocationCount() - before;

            EXPECT_EQ(spent, 0u) << "a second description reached the heap " << spent << " times";

            // And it answered, rather than reaching the heap not at all by doing nothing.
            EXPECT_TRUE(described.getRefusals().empty());
            ASSERT_EQ(described.getDescriptions().size(), std::size_t{ 2 });
        }

        /// A slot the scene has given up is described by nobody, and the gap it leaves is survived.
        ///
        /// `SceneDesc` empties a freed slot's path and leaves it in the table until something takes
        /// it over. Describing one would build an image, a shading map and a descriptor write for a
        /// slot no material can reach — and count it as a texture that could not be read, which is
        /// what made a departing cell look like a broken one.
        ///
        /// **The freed slot is first here on purpose.** What comes out is no longer one description
        /// per entry of the table, so a backend taking position for slot would put every texture
        /// above the gap one place too low — and it did, until the array was told to honour the slot
        /// each description carries.
        TEST(RtxTextureBuilderTest, aFreedSlotIsNotDescribedAndTheOthersKeepTheirSlots)
        {
            VFS::Manager vfs;
            Resource::ImageManager images(&vfs, 0);

            // Two models, because a texture is only given back when the last material naming it is:
            // `release` answers the ordinary frame by comparing the mesh and material counts and
            // returning before it frees anything at all.
            Rtx::SceneDesc scene;
            const Testing::Model going = Testing::addModel(scene, VFS::Path::NormalizedView("textures/freed.dds"));
            const Testing::Model staying = Testing::addModel(scene, VFS::Path::NormalizedView("textures/named.dds"));

            const std::array<Rtx::Index, 1> keptMeshes{ staying.mMesh };
            const std::array<Rtx::Index, 1> keptMaterials{ staying.mMaterial };

            // Dropped before the sweep, as the walk drops a placement before it lets go of the rows
            // it stood on: the sweep asserts that nothing stands on what it frees.
            scene.placements().drop(going.mPlacement, Rtx::Stander::Walk);
            ASSERT_TRUE(scene.release(keptMeshes, keptMaterials));
            ASSERT_EQ(going.mTexture, 0u) << "the gap has to be below something to be a gap";
            ASSERT_TRUE(scene.textures().isFree(going.mTexture));
            ASSERT_FALSE(scene.textures().isFree(staying.mTexture));

            // The VFS is empty, so the one that is described does not resolve — which is the other
            // half of the statement: a slot that named a file and failed at it is a failure, and a
            // slot that names nothing is not one.
            const auto check = [&](const SceneTextures& described, const char* which) {
                ASSERT_EQ(described.getDescriptions().size(), std::size_t{ 1 }) << which;
                EXPECT_EQ(described.getDescriptions()[0].mSlot, staying.mTexture) << which;
                EXPECT_EQ(described.getDescriptions()[0].mName, "stand-in") << which;
                ASSERT_EQ(described.getRefusals().size(), 1u) << which;
                EXPECT_EQ(described.getRefusals()[0].mKind, Refused::Texture) << which;
                EXPECT_EQ(described.getRefusals()[0].mWhy, "no image reads from the file") << which;
            };

            const std::array<Rtx::Index, 2> both{ going.mTexture, staying.mTexture };

            // One loader for both, which is how the uploader holds it: the second call clears what
            // the first left and answers on its own.
            SceneTextures described;
            described.describe(scene, images, both);
            check(described, "described by arrival");

            described.describeAll(scene, images);
            check(described, "described from the whole table");
        }

        /// A sprite's lighting bake is a slot of the same table, made on the device from the slot
        /// of the file its key names: the description carries the source's slot and no bytes. Where
        /// the table holds no such file there is no alpha to bake from, so it gets the stand-in a
        /// sprite that could not be read gets, counted once.
        TEST(RtxTextureBuilderTest, aSpriteLightBakeNamesItsSourcesSlotAndOneWithNoneGetsTheStandIn)
        {
            constexpr VFS::Path::NormalizedView smoke("textures/tx_smoke.dds");

            VFS::Manager vfs;
            Resource::ImageManager images(&vfs, 0);

            Rtx::SceneDesc scene;
            const Rtx::Index bake = scene.textures().addBaked(SpriteLightMap::keyFor(smoke));

            SceneTextures described;
            described.describeAll(scene, images);
            ASSERT_EQ(described.getDescriptions().size(), std::size_t{ 1 });
            EXPECT_EQ(described.getDescriptions()[0].mSlot, bake);
            EXPECT_EQ(described.getDescriptions()[0].mName, "stand-in");
            EXPECT_EQ(described.getDescriptions()[0].mSource, Rtx::TextureSource::StandIn);
            EXPECT_EQ(described.getDescriptions()[0].mFrom, Rtx::sNoIndex);
            ASSERT_EQ(described.getRefusals().size(), 1u);
            EXPECT_EQ(described.getRefusals()[0].mWhy, "the texture it bakes is no longer held");

            // The emitter holds the source under a wrap of its own beside the bake; the bake finds
            // it whichever wrap that was, and is then a bake and not a texture.
            const Rtx::Index source = scene.textures().add(smoke, Rtx::TextureWrap::ClampS);
            described.describe(scene, images, std::span(&bake, 1));
            ASSERT_EQ(described.getDescriptions().size(), std::size_t{ 1 });
            EXPECT_EQ(described.getDescriptions()[0].mSlot, bake);
            EXPECT_EQ(described.getDescriptions()[0].mSource, Rtx::TextureSource::SpriteBake);
            EXPECT_EQ(described.getDescriptions()[0].mFrom, source);
            EXPECT_TRUE(described.getDescriptions()[0].mBytes.empty()) << "a bake carries no bytes";
            EXPECT_TRUE(described.getDescriptions()[0].mLevels.empty()) << "a bake is shaped like its source";
            EXPECT_TRUE(described.getDescriptions()[0].hasNeutralShading());
            EXPECT_TRUE(described.getRefusals().empty());
        }

        /// A chunk's flattened ground is a slot the queue gave out: the description carries the
        /// chunk the device sums into it and no bytes. The same slot described with no queue, or
        /// by one that did not give it out this frame, is a baked name nothing can read and gets
        /// the stand-in.
        TEST(RtxTextureBuilderTest, aCompositeNamesItsChunkAndOneNoQueueGaveOutGetsTheStandIn)
        {
            VFS::Manager vfs;
            Resource::ImageManager images(&vfs, 0);

            Rtx::SceneDesc scene;
            const std::array<Rtx::MaterialLayer, 2> layers{ Rtx::MaterialLayer{ .mDiffuse = 0 },
                Rtx::MaterialLayer{ .mDiffuse = 1 } };
            scene.textures().add(VFS::Path::NormalizedView("textures/under.dds"));
            scene.textures().add(VFS::Path::NormalizedView("textures/over.dds"));
            Rtx::Material chunk;
            chunk.mKind = Rtx::MaterialKind::Terrain;
            chunk.mFlatten = true;
            chunk.mLayers = scene.materials().addLayers(layers);
            const Rtx::Index material = scene.addMaterial(chunk);

            Rtx::CompositeQueue queue;
            ASSERT_EQ(queue.advance(scene), 1u);
            const Rtx::Index composite = scene.materials().getRows()[material].mDiffuse;
            ASSERT_NE(composite, Rtx::sNoIndex);

            SceneTextures described;
            described.describe(scene, images, std::span(&composite, 1), &queue);
            ASSERT_EQ(described.getDescriptions().size(), std::size_t{ 1 });
            EXPECT_EQ(described.getDescriptions()[0].mSlot, composite);
            EXPECT_EQ(described.getDescriptions()[0].mSource, Rtx::TextureSource::GroundComposite);
            EXPECT_EQ(described.getDescriptions()[0].mFrom, material);
            EXPECT_EQ(described.getDescriptions()[0].mFormat, Rtx::TextureFormat::Rgba8Srgb);
            EXPECT_TRUE(described.getDescriptions()[0].mBytes.empty()) << "a composite carries no bytes";
            EXPECT_TRUE(described.getDescriptions()[0].mLevels.empty()) << "a composite is shaped by the pass";
            EXPECT_TRUE(described.getDescriptions()[0].hasNeutralShading());
            EXPECT_TRUE(described.getRefusals().empty());

            queue.releaseFinished();
            described.describe(scene, images, std::span(&composite, 1), &queue);
            ASSERT_EQ(described.getDescriptions().size(), std::size_t{ 1 });
            EXPECT_EQ(described.getDescriptions()[0].mSource, Rtx::TextureSource::StandIn);
            EXPECT_EQ(described.getDescriptions()[0].mFrom, Rtx::sNoIndex);
            EXPECT_EQ(described.getDescriptions()[0].mName, "stand-in");
            ASSERT_EQ(described.getRefusals().size(), 1u);
            EXPECT_EQ(described.getRefusals()[0].mWhy, "no ground was queued to flatten into it");
        }

        /// A file that carried one level is described as that level and nothing more: the chain is
        /// the device's to make, `mipchain.comp`, and a describe reads no texel for it.
        TEST(RtxTextureBuilderTest, aFileWithoutAChainIsDescribedAsItsOneLevel)
        {
            constexpr VFS::Path::NormalizedView path("textures/tx_read.dds");

            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setFileName(std::string(path.value()));
            image->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            std::fill_n(image->data(), image->getTotalSizeInBytes(), static_cast<unsigned char>(128));

            VFS::Manager vfs;
            Testing::HeldImages images(&vfs, 0);
            images.hold(path, image);

            Rtx::SceneDesc scene;
            Testing::addModel(scene, path);

            SceneTextures described;
            described.describeAll(scene, images);
            ASSERT_EQ(described.getDescriptions().size(), 1u);
            EXPECT_EQ(described.getDescriptions()[0].mLevels.size(), 1u) << "the file's own level and no chain";
            EXPECT_TRUE(described.getDescriptions()[0].mCompleteChain) << "the chain is the device's to make";
            EXPECT_EQ(described.getDescriptions()[0].mBytes.data(), reinterpret_cast<const std::byte*>(image->data()))
                << "the file's own bytes, spanned and not copied";
            EXPECT_EQ(described.getDescriptions()[0].mSource, Rtx::TextureSource::File);
            EXPECT_FALSE(described.getDescriptions()[0].hasNeutralShading()) << "a file is estimated on the device";
        }
    }
}

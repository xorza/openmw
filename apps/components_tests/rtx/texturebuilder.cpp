#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Image>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/resource/imagemanager.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/mipchain.hpp>
#include <components/rtx/preparedtexture.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shadingmap.hpp>
#include <components/rtx/spritelight.hpp>
#include <components/rtx/texturebuilder.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "allocations.hpp"
#include "heldimages.hpp"

namespace Rtx
{
    namespace
    {
        /// A mesh, a material and the texture it names, which is how a model arrives.
        struct Model
        {
            Rtx::Index mMesh = 0;
            Rtx::Index mMaterial = 0;
            Rtx::Index mTexture = 0;
        };

        /// One triangle and one material naming `texture`, so all three arrive together.
        Model addModel(Rtx::SceneDesc& scene, VFS::Path::NormalizedView texture)
        {
            const osg::Vec3f positions[3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
            const osg::Vec3f normals[3] = { { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 } };
            const osg::Vec2f uvs[3] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
            const std::uint32_t indices[3] = { 0, 1, 2 };

            Model made;
            made.mMesh = scene.addMesh(
                MeshArrays{ .mPositions = positions, .mNormals = normals, .mTexCoords = uvs, .mIndices = indices });
            made.mTexture = scene.textures().add(texture);

            Rtx::Material material;
            material.mDiffuse = made.mTexture;
            made.mMaterial = scene.materials().add(material);
            scene.addInstance(Rtx::MeshInstance{ .mMesh = made.mMesh, .mMaterial = made.mMaterial });

            return made;
        }

        /// One block's worth of image in `format`, which is all the description reads beyond it.
        osg::ref_ptr<osg::Image> makeBlock(GLenum format)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setFileName("textures/tx_test.dds");
            image->allocateImage(4, 4, 1, format, GL_UNSIGNED_BYTE);
            return image;
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

            EXPECT_EQ(describeImage(*makeBlock(GL_COMPRESSED_RGB_S3TC_DXT1_EXT), levels).mFormat,
                Rtx::TextureFormat::Bc1RgbaSrgb);
            EXPECT_EQ(describeImage(*makeBlock(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT), levels).mFormat,
                Rtx::TextureFormat::Bc1RgbaSrgb);
        }

        /// The formats Morrowind actually ships, kept apart. DXT3 carrying its own alpha is what
        /// the handful of soft-edged masks in the game are stored as.
        TEST(RtxTextureBuilderTest, theOtherBlockFormatsKeepTheirOwnMapping)
        {
            std::vector<Rtx::MipLevel> levels;

            EXPECT_EQ(describeImage(*makeBlock(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT), levels).mFormat,
                Rtx::TextureFormat::Bc2Srgb);
            EXPECT_EQ(describeImage(*makeBlock(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT), levels).mFormat,
                Rtx::TextureFormat::Bc3Srgb);
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
            levels.reserve(first->getNumMipmapLevels() + second->getNumMipmapLevels());

            const Rtx::TextureData a = describeImage(*first, levels);
            const Rtx::TextureData b = describeImage(*second, levels);

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

            EXPECT_EQ(describeImage(*makeBlock(GL_RGBA), levels).mFormat, Rtx::TextureFormat::Rgba8Srgb);
            EXPECT_EQ(describeImage(*makeBlock(GL_BGRA), levels).mFormat, Rtx::TextureFormat::Bgra8Srgb);

            // **Display-encoded, which every content format is.** The one uncompressed format that
            // is not exists for tests asserting an exact texel, and a cloud texture read through it
            // would come out with its transfer function applied twice.
            EXPECT_TRUE(Rtx::isSrgb(Rtx::TextureFormat::Rgba8Srgb));
            EXPECT_TRUE(Rtx::isSrgb(Rtx::TextureFormat::Bgra8Srgb));
        }

        /// A format this still cannot upload fails by name rather than uploading noise.
        ///
        /// Three-channel spellings are refused deliberately: uploading one would need the fourth
        /// channel written in, which means owning a buffer, and nothing this game ships stores an
        /// opaque texture without one.
        TEST(RtxTextureBuilderTest, aFormatWithNoAlphaChannelIsRefusedAndSaysWhich)
        {
            std::vector<Rtx::MipLevel> levels;
            EXPECT_THROW(describeImage(*makeBlock(GL_RGB), levels), Rtx::Error);
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

            Rtx::SceneDesc scene;
            addModel(scene, path);

            SceneTextures described;
            described.describeAll(scene.getTables(), images);
            ASSERT_EQ(described.getUnreadable(), 0u) << "the image did not come back from the cache";
            ASSERT_EQ(described.getDescriptions().size(), std::size_t{ 1 });

            // Four texels across and one level in the file, so `MipChain` builds the rest: 4, 2 and
            // 1. That puts the pooled chain under the guard below rather than only the flat tables.
            ASSERT_EQ(described.getDescriptions()[0].mLevels.size(), std::size_t{ 3 });

            const std::size_t before = Testing::getAllocationCount();
            described.describeAll(scene.getTables(), images);
            const std::size_t spent = Testing::getAllocationCount() - before;

            EXPECT_EQ(spent, 0u) << "a second description reached the heap " << spent << " times";

            // And it answered, rather than reaching the heap not at all by doing nothing.
            EXPECT_EQ(described.getUnreadable(), 0u);
            ASSERT_EQ(described.getDescriptions().size(), std::size_t{ 1 });
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
            const Model going = addModel(scene, VFS::Path::NormalizedView("textures/freed.dds"));
            const Model staying = addModel(scene, VFS::Path::NormalizedView("textures/named.dds"));

            const std::array<Rtx::Index, 1> keptMeshes{ staying.mMesh };
            const std::array<Rtx::Index, 1> keptMaterials{ staying.mMaterial };

            ASSERT_TRUE(scene.release(keptMeshes, keptMaterials));
            ASSERT_EQ(going.mTexture, 0u) << "the gap has to be below something to be a gap";
            ASSERT_TRUE(scene.getTables().mTextures.isFree(going.mTexture));
            ASSERT_FALSE(scene.getTables().mTextures.isFree(staying.mTexture));

            // The VFS is empty, so the one that is described does not resolve — which is the other
            // half of the statement: a slot that named a file and failed at it is a failure, and a
            // slot that names nothing is not one.
            const auto check = [&](const SceneTextures& described, const char* which) {
                ASSERT_EQ(described.getDescriptions().size(), std::size_t{ 1 }) << which;
                EXPECT_EQ(described.getDescriptions()[0].mSlot, staying.mTexture) << which;
                EXPECT_EQ(described.getDescriptions()[0].mName, "unreadable") << which;
                EXPECT_EQ(described.getUnreadable(), 1u) << which;
            };

            const std::array<Rtx::Index, 2> both{ going.mTexture, staying.mTexture };

            // One loader for both, which is how the uploader holds it: the second call clears what
            // the first left and answers on its own.
            SceneTextures described;
            described.describe(scene.getTables(), images, both);
            check(described, "described by arrival");

            described.describeAll(scene.getTables(), images);
            check(described, "described from the whole table");
        }

        /// A sprite's lighting bake is a slot of the same table, made from the file its key names —
        /// and where that file cannot be read there is no alpha to bake, so it gets the stand-in the
        /// sprite itself gets, counted once.
        TEST(RtxTextureBuilderTest, aSpriteLightBakeWithNoSourceToReadGetsTheStandIn)
        {
            VFS::Manager vfs;
            Resource::ImageManager images(&vfs, 0);

            Rtx::SceneDesc scene;
            const Rtx::Index bake
                = scene.textures().addBaked(SpriteLightMap::keyFor(VFS::Path::NormalizedView("textures/tx_smoke.dds")));

            SceneTextures described;
            described.describeAll(scene.getTables(), images);
            ASSERT_EQ(described.getDescriptions().size(), std::size_t{ 1 });
            EXPECT_EQ(described.getDescriptions()[0].mSlot, bake);
            EXPECT_EQ(described.getDescriptions()[0].mName, "unreadable");
            EXPECT_EQ(described.getUnreadable(), 1u);
        }
        /// A fake of what the static ring answers: one image, read ahead of the frame.
        class OneReading final : public TextureReadings
        {
        public:
            PreparedTexture mTexture;

            const PreparedTexture* find(const osg::Image& image) const override
            {
                return &image == mTexture.mImage.get() ? &mTexture : nullptr;
            }
        };

        /// A describe takes a reading's chain and shading over building its own, and builds its own
        /// where nothing read the image ahead of it.
        TEST(RtxTextureBuilderTest, aReadingIsTakenOverABuildAndAMissIsBuilt)
        {
            constexpr VFS::Path::NormalizedView path("textures/tx_read.dds");

            // Four by four, one level, so the builder has a chain to build and the reading has one to
            // carry: 4x4, 2x2 and 1x1.
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setFileName(std::string(path.value()));
            image->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            std::fill_n(image->data(), image->getTotalSizeInBytes(), static_cast<unsigned char>(128));

            VFS::Manager vfs;
            Testing::HeldImages images(&vfs, 0);
            images.hold(path, image);

            Rtx::SceneDesc scene;
            addModel(scene, path);

            OneReading reading;
            reading.mTexture.mImage = image;
            std::vector<Rtx::MipLevel> levels;
            reading.mTexture.mChain.build(describeImage(*image, levels));
            ASSERT_FALSE(reading.mTexture.mChain.isEmpty());
            reading.mTexture.mShading.fill(2.0f);
            reading.mTexture.mReadable = true;

            SceneTextures taken;
            taken.describeAll(scene.getTables(), images, nullptr, &reading);
            ASSERT_EQ(taken.getDescriptions().size(), 1u);
            EXPECT_EQ(taken.getDescriptions()[0].mLevels.size(), 3u) << "the reading's chain, down to one texel";
            EXPECT_EQ(taken.getDescriptions()[0].mShading[0], 2.0f) << "the reading's estimate, not one made here";
            EXPECT_EQ(taken.getDescriptions()[0].mShading.size(), Rtx::ShadingMap::sCells);

            SceneTextures built;
            built.describeAll(scene.getTables(), images);
            ASSERT_EQ(built.getDescriptions().size(), 1u);
            EXPECT_EQ(built.getDescriptions()[0].mLevels.size(), 3u) << "built here, to the same chain";
            EXPECT_NEAR(built.getDescriptions()[0].mShading[0], 1.0f, 0.01f)
                << "a flat grey estimates to one everywhere, which is not the reading's two";

            // An unreadable reading is no reading: the miss path runs and the stand-in follows.
            reading.mTexture.mReadable = false;
            SceneTextures again;
            again.describeAll(scene.getTables(), images, nullptr, &reading);
            EXPECT_NEAR(again.getDescriptions()[0].mShading[0], 1.0f, 0.01f);
        }

    }
}

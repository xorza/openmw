#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Image>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/resource/imagemanager.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/texturebuilder.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

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
            made.mMesh = scene.addMesh(positions, normals, uvs, indices);
            made.mTexture = scene.addTexture(texture);

            Rtx::Material material;
            material.mDiffuse = made.mTexture;
            made.mMaterial = scene.addMaterial(material);
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
            ASSERT_TRUE(scene.isTextureFree(going.mTexture));
            ASSERT_FALSE(scene.isTextureFree(staying.mTexture));

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
            check(SceneTextures(scene, images, both), "described by arrival");
            check(SceneTextures(scene, images), "described from the whole table");
        }
    }
}

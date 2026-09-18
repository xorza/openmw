#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/rtx/camera.hpp>
#include <components/rtx/frameimage.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/look.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/shadingmap.hpp>
#include <components/rtx/slot.hpp>
#include <components/rtx/surface.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/frameslots.hpp>
#include <components/rtxvulkan/scenebuffers.hpp>
#include <components/rtxvulkan/spritebin.hpp>
#include <components/vfs/pathutil.hpp>

#include "../geometry.hpp"
#include "../harness.hpp"
#include "../testtexture.hpp"
#include "fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// The one test in this file that asks the tables rather than the picture, so it wants a
        /// device where the others want a renderer.
        struct RtxSceneTableTest : DeviceTest
        {
        };

        /// A scene with nothing in it still has a table at every address the frame carries.
        ///
        /// **A null handle at a descriptor is undefined at the dispatch**, and undefined here meant a
        /// lost device five seconds in, intermittently, with no message — the loop waited forever on
        /// a fence that would never signal. Three tables were doing it: a scene with no textures
        /// asked for no shading maps and a frame with no sprites asked for no tiles, and the rule
        /// every table was grown by read "grow if what is wanted does not fit", which never makes one
        /// at all when nothing is wanted. An address of nought in the frame block is the same
        /// mistake one step later, and the device says even less about it.
        ///
        /// The fix is that the owner opens every table when it is built rather than when something
        /// writes to one, because the write is exactly what does not happen. This is the assertion
        /// that says so, and it is the one that would have caught it.
        TEST_F(RtxSceneTableTest, aSceneWithNothingInItStillAddressesATableForEverythingDeclared)
        {
            Device& device = getDevice();
            CommandPool& pool = getPool();

            const SceneDesc empty;

            // No sprites, so no tiles, and that table used to come out as `VK_NULL_HANDLE`.
            Batch setup(pool);
            const SceneBuffers buffers(device, setup, empty, {}, 1);
            setup.flush();

            // **Every table this hands out, and not the three that were caught.** The rule was the
            // same for all of them; which ones happened to be empty on the day is not what decides
            // whether they are covered. An address of nought is a table bound as nothing, and an
            // address off what its reference claims is a load the device may split or fault on, with
            // no message either.
            Shaders::GpuTables addressed{};
            buffers.describeTables(FrameSlot{}, addressed);

            // The two the trace's own bin writes, which it hands out the same way, whether or not
            // anything was ever binned into it.
            const SpriteBin bin(device);
            addressed.mSprites = bin.getSpritesAddress();
            addressed.mSpriteTileList = bin.getTileListAddress();

            struct Named
            {
                const char* mWhat;
                std::uint64_t mAddress;
                std::uint32_t mAlign;
            };
            const std::array<Named, 12> named{ {
                { "the normal blocks", addressed.mNormalBlocks, Shaders::TABLE_ALIGN_BLOCKS },
                { "the texture coordinate blocks", addressed.mTexCoordBlocks, Shaders::TABLE_ALIGN_BLOCKS },
                { "the meshes", addressed.mMeshes, Shaders::TABLE_ALIGN_ROWS },
                { "the instance rows", addressed.mInstances, Shaders::TABLE_ALIGN_ROWS },
                { "the materials", addressed.mMaterials, Shaders::TABLE_ALIGN_ROWS },
                { "the terrain layers", addressed.mLayers, Shaders::TABLE_ALIGN_LAYERS },
                { "the blend masks", addressed.mMasks, Shaders::TABLE_ALIGN_ROWS },
                { "the lights", addressed.mLights, Shaders::TABLE_ALIGN_ROWS },
                { "the light grid's list", addressed.mLightList, Shaders::TABLE_ALIGN_ROWS },
                { "the sprites", addressed.mSprites, Shaders::TABLE_ALIGN_ROWS },
                { "the emitters", addressed.mEmitters, Shaders::TABLE_ALIGN_ROWS },
                { "the sprite tiles' list", addressed.mSpriteTileList, Shaders::TABLE_ALIGN_ROWS },
            } };

            for (const Named& table : named)
            {
                EXPECT_NE(table.mAddress, 0u) << table.mWhat;
                EXPECT_EQ(table.mAddress % table.mAlign, 0u) << table.mWhat << " at " << table.mAddress;
            }
        }

        /// One renderer, three scenes, and the number of textures changing under it.
        ///
        /// **A texture arriving must not disturb the ones already uploaded.**
        ///
        /// The array is bindless and a material indexes it by position, so an append that wrote its
        /// descriptor at the wrong element would leave a surface sampling somebody else's texture —
        /// which reads as a plausible picture, not as an error. Rebuilding the whole array is what
        /// this replaces, and it was measured at 150 to 225 ms against 12 for every acceleration
        /// structure in the scene: the game spent nine tenths of every cell change there.
        TEST_F(RtxVisibilityTest, aTextureAppendedLandsInItsOwnSlotAndLeavesTheRestAlone)
        {
            constexpr std::uint32_t size = 32;
            constexpr std::size_t centre = centreValueOf(size);

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;

            constexpr std::array<std::uint8_t, 4> redTexel{ 255, 0, 0, 255 };
            constexpr std::array<std::uint8_t, 4> blueTexel{ 0, 0, 255, 255 };
            SceneDesc scene;
            const Index mesh
                = scene.addMesh(MeshArrays{ .mPositions = sWallQuad, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
            const Index red
                = scene.addMaterial(Material{ .mDiffuse = scene.textures().add(VFS::Path::NormalizedView("red.dds")) });
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = red });

            mRenderer->resize(size, size);
            const TextureData first = describeTexel(redTexel, 0);
            mRenderer->setScene(Rtx::SceneSlot::world(), scene, std::span(&first, 1));
            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });

            // **A hue rather than a pair of exact bytes.** The tone curve rolls a saturated colour
            // off short of the display's end and carries it toward white as it goes, so a full-red
            // texel arrives at 241 with a little blue under it rather than at 255 with none. Which
            // texture a wall wears is still the question, and the channels still answer it.
            std::vector<std::uint8_t> shown;
            const auto wearsRed = [&shown](std::size_t at) { return shown[at] > 200 && shown[at + 2] < 100; };
            const auto wearsBlue = [&shown](std::size_t at) { return shown[at] < 100 && shown[at + 2] > 200; };

            mRenderer->readPixels(shown);
            ASSERT_TRUE(wearsRed(centre)) << "the wall did not start out red";
            ASSERT_EQ(mRenderer->describeHeld(Rtx::SceneSlot::world()).mTextureCount, 1u);

            // A second texture and a second material, on a wall nearer the eye. The mesh table is
            // untouched, so this is the append path and not a rebuild.
            const Index blue = scene.addMaterial(
                Material{ .mDiffuse = scene.textures().add(VFS::Path::NormalizedView("blue.dds")) });
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::translate(0.0f, -50.0f, 0.0f), .mMesh = mesh, .mMaterial = blue });

            // **The slot the scene gave it**, which is what an arrival now carries: a texture is
            // written where it belongs rather than after whatever is already there.
            const Index blueTexture = scene.materials().getRows()[blue].mDiffuse;
            const TextureData second = describeTexel(blueTexel, blueTexture);
            mRenderer->extendScene(Rtx::SceneSlot::world(), scene, std::span(&second, 1));
            EXPECT_EQ(mRenderer->describeHeld(Rtx::SceneSlot::world()).mTextureCount, 2u);

            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });
            mRenderer->readPixels(shown);

            // The nearer wall wears the texture that was appended, which is only true if its
            // descriptor went to element one. Written to element zero it would come out red, and
            // written nowhere it would come out as whatever the array holds there — both plausible.
            EXPECT_TRUE(wearsBlue(centre)) << "the near wall wears the texture that was appended";

            // And the first texture is still where it was: move the near wall out of the way and the
            // one behind it has to be red again, sampled from a descriptor nothing rewrote.
            scene.placements().drop(1, Stander::Walk);
            mRenderer->placeScene(Rtx::SceneSlot::world(), scene);
            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });
            mRenderer->readPixels(shown);

            EXPECT_TRUE(wearsRed(centre)) << "the texture already uploaded was disturbed by the append";

            // **And a table with a hole at the end of it.** Letting the near wall's material go
            // frees the texture it wore, and the slot stays in the scene's table until something
            // takes it over — so an array built from what is left has to be as long as the table
            // rather than as long as the descriptions. Stopping at the last one written also stops
            // `SceneUploader` recognising its own scene, and every frame after this would build the
            // world again from nothing.
            const std::array<Index, 1> keptMeshes{ mesh };
            const std::array<Index, 1> keptMaterials{ red };
            ASSERT_TRUE(scene.release(keptMeshes, keptMaterials));
            ASSERT_TRUE(scene.textures().isFree(blueTexture));
            ASSERT_EQ(scene.textures().getRows().size(), 2u) << "the table does not shrink";

            mRenderer->setScene(Rtx::SceneSlot::world(), scene, std::span(&first, 1));

            EXPECT_EQ(mRenderer->describeHeld(Rtx::SceneSlot::world()).mTextureCount, 2u)
                << "the array stopped at the last texture it was handed rather than at the table";

            // **And what the report says is what is stood, not how long the table is.** The two are
            // one number until something is freed, which is why a still never showed the difference
            // and a route reported a hundred textures it was not holding. One texel of four bytes and
            // the map beside it, two bytes a cell, is the whole of what is left here.
            constexpr std::size_t map = std::size_t{ Shaders::SHADING_EXTENT } * Shaders::SHADING_EXTENT * 2;
            EXPECT_EQ(mRenderer->getSceneStats().mTextureCount, 1u);
            EXPECT_EQ(mRenderer->getSceneStats().mTextureBytes, redTexel.size() + map);

            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });
            mRenderer->readPixels(shown);

            EXPECT_TRUE(wearsRed(centre)) << "the texture that survived lost its slot";

            // **And the same table with the hole at the bottom of it.** A wall in front wearing a
            // texture the scene has put back into the slot the last one gave up, and then the far
            // wall's material goes: what is left is one description naming slot one over a slot zero
            // nothing stands in. An array numbering its descriptions by position would write it at
            // zero, and the wall would sample a descriptor nobody ever wrote.
            const Index again = scene.addMaterial(
                Material{ .mDiffuse = scene.textures().add(VFS::Path::NormalizedView("blue.dds")) });
            ASSERT_EQ(scene.materials().getRows()[again].mDiffuse, blueTexture) << "the freed slot was not taken over";

            scene.placements().drop(0, Stander::Walk);
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = again });

            const std::array<Index, 1> keptAgain{ again };
            ASSERT_TRUE(scene.release(keptMeshes, keptAgain));
            ASSERT_TRUE(scene.textures().isFree(0u));

            mRenderer->setScene(Rtx::SceneSlot::world(), scene, std::span(&second, 1));
            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });
            mRenderer->readPixels(shown);

            EXPECT_TRUE(wearsBlue(centre)) << "the description landed at its position rather than its slot";
        }

        /// **The pass is built once and kept, because building one compiles a shader** — so the set
        /// layout the bindless array declares cannot depend on how many textures a cell holds. It
        /// did: a scene with a different count produced a layout the kept pipeline layout would not
        /// accept, and the frame came out looking right while the layers said
        /// `VUID-vkCmdBindDescriptorSets-pDescriptorSets-00358`. That is why the two tests that
        /// caught it passed when either was run on its own.
        ///
        /// Half the assertion is the fixture's: `TearDown` fails on any validation error, and this
        /// is a defect that shows up there before it shows up in a pixel.
        TEST_F(RtxVisibilityTest, aSceneChangingItsTextureCountStillBindsAgainstTheKeptPass)
        {
            constexpr std::uint32_t size = 32;
            constexpr std::size_t centre = centreValueOf(size);

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;

            // No textures at all, so the array is allocated with nothing in it. The untextured
            // material's 0.5 encoded: `1.055 * 0.5^(1/2.4) - 0.055` is 0.735, or 187 of 255.
            std::vector<std::uint8_t> plain;
            EXPECT_EQ(countHits(makeWall(), {}, camera, size, plain), size * size);
            EXPECT_NEAR(plain[centre], 187, 1);

            // The same wall carrying two textures. Two and not one, because an empty array is
            // allocated a slot anyway — a scene of none and a scene of one ask for the same thing,
            // and it takes a second texture for the counts to differ at all.
            //
            // Red is the diffuse and is what the albedo view shows; the green is emissive, which
            // that view does not read, so it is here to be counted rather than to be seen.
            constexpr std::array<std::uint8_t, 4> redTexel{ 255, 0, 0, 255 };
            constexpr std::array<std::uint8_t, 4> greenTexel{ 0, 255, 0, 255 };
            const std::array<TextureData, 2> textures{ describeTexel(redTexel), describeTexel(greenTexel) };

            // The same wall, and it needs texture coordinates that `makeWall` has no use for.
            SceneDesc textured;
            const Index mesh = textured.addMesh(
                MeshArrays{ .mPositions = sWallQuad, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
            const Index material = textured.addMaterial(
                Material{ .mDiffuse = textured.textures().add(VFS::Path::NormalizedView("red.dds")),
                    .mEmissive = textured.textures().add(VFS::Path::NormalizedView("green.dds")) });
            textured.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

            std::vector<std::uint8_t> shown;
            EXPECT_EQ(countHits(textured, textures, camera, size, shown), size * size);
            EXPECT_EQ(shown[centre], 255) << "red";
            EXPECT_EQ(shown[centre + 1], 0) << "green";
            EXPECT_EQ(shown[centre + 2], 0) << "blue";

            // And back down to none, which was as broken as the way up and is the direction a cell
            // change actually takes when a player walks out of a rich interior.
            std::vector<std::uint8_t> again;
            EXPECT_EQ(countHits(makeWall(), {}, camera, size, again), size * size);
            EXPECT_EQ(again, plain);
        }

        /// A mesh in the second block of the shared buffers is shaded out of the second block.
        ///
        /// **Nothing the game loads reaches this.** Balmora is 165,536 vertices and 589,869 indices
        /// against blocks of 262,144 and 1,048,576, so every scene this fork has ever rendered lives
        /// in block zero and `id / VERTEX_BLOCK` has never been anything but zero. Blocking exists
        /// for what happens when it is not, and the only thing that can say whether that works is a
        /// scene built to cross the boundary.
        ///
        /// The filler is one degenerate triangle carrying a whole block of vertices, and it is never
        /// instanced — so the two scenes hand the tracer the same instance, the same material and
        /// the same texture, and differ in nothing but where the wall's vertices sit. The allocator
        /// will not let a run straddle a block, so a mesh that does not fit the tail starts the next
        /// one.
        TEST_F(RtxVisibilityTest, aMeshInTheSecondBlockIsShadedOutOfTheSecondBlock)
        {
            constexpr std::uint32_t size = 64;
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            // **Lit, and lit from the side.** An unlit wall is the same black whatever its normals
            // and its texture came to, which is a test that cannot fail. The sun crosses the face
            // rather than facing it, so the tilt below is the whole of what decides each pixel.
            camera.mSun = Shaders::sunSource(osg::Vec3f(-1.0f, -0.2f, 0.0f), osg::Vec3f(3.0f, 3.0f, 3.0f));

            // **Four different texels, so a texture coordinate read out of the wrong block shows.**
            // A flat texture gives the same pixel whatever the coordinates came to, and this test
            // would then pass with the coordinate table unread.
            constexpr std::array<std::uint8_t, 16> corners{
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
                0,
                255,
            };
            constexpr MipLevel one{ 0, 2, 2 };
            const TextureData painted{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = 2,
                .mHeight = 2,
                .mBytes = std::as_bytes(std::span(corners)),
                .mLevels = std::span(&one, 1),
            };

            // **Tilted away from the face they sit on, for the same reason.** A shading normal equal
            // to the geometric one is a normal the shader can lose without the picture moving: it
            // falls back to the geometry whenever what it read is degenerate, which is exactly what
            // an unwritten block holds.
            const std::array<osg::Vec3f, 4> quadNormals{
                osg::Vec3f(-0.6f, -0.8f, 0.0f),
                osg::Vec3f(0.6f, -0.8f, 0.0f),
                osg::Vec3f(0.6f, -0.8f, 0.0f),
                osg::Vec3f(-0.6f, -0.8f, 0.0f),
            };

            const auto addWall = [&](SceneDesc& scene) {
                const Index mesh = scene.addMesh(MeshArrays{ .mPositions = sWallQuad,
                    .mNormals = quadNormals,
                    .mTexCoords = sQuadUv,
                    .mIndices = sQuadIndices });
                const Index material = scene.addMaterial(
                    Material{ .mDiffuse = scene.textures().add(VFS::Path::NormalizedView("corners.dds")) });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });
                return mesh;
            };

            SceneDesc single;
            const Index alone = addWall(single);
            ASSERT_EQ(single.meshes().getRows()[alone].mVertices.mOffset, 0u);

            // **One filler that fills both blocks**, because the vertex and index tables are blocked
            // at different sizes and a mesh pushed past one is not thereby past the other.
            SceneDesc crossed;
            const std::vector<osg::Vec3f> fillerVertices(SceneDesc::sVertexBlock, osg::Vec3f(0.0f, 0.0f, 0.0f));

            // The largest whole number of triangles a block holds, so what is left of it is one
            // index and the wall's six cannot fit.
            std::vector<std::uint32_t> fillerIndices(SceneDesc::sIndexBlock / 3 * 3);
            for (std::size_t at = 0; at < fillerIndices.size(); ++at)
                fillerIndices[at] = static_cast<std::uint32_t>(at % 3);

            crossed.addMesh(MeshArrays{ .mPositions = fillerVertices, .mIndices = fillerIndices });
            const Index beyond = addWall(crossed);

            // Hand-computed: the filler is a whole vertex block, so the wall starts the next one;
            // and 1,048,575 of 1,048,576 indices leaves a tail of one, which six will not fit into.
            // Asserted, because a test whose subject quietly moved back into block zero would pass
            // while testing nothing.
            ASSERT_EQ(crossed.meshes().getRows()[beyond].mVertices.mOffset, SceneDesc::sVertexBlock);
            ASSERT_EQ(crossed.meshes().getRows()[beyond].mIndices.mOffset, SceneDesc::sIndexBlock);

            std::vector<std::uint8_t> alonePixels;
            std::vector<std::uint8_t> crossedPixels;
            EXPECT_EQ(countHits(single, std::span(&painted, 1), camera, size, alonePixels), size * size);
            EXPECT_EQ(countHits(crossed, std::span(&painted, 1), camera, size, crossedPixels), size * size);

            EXPECT_EQ(crossedPixels, alonePixels)
                << "the wall shaded differently once its vertices moved into the second block";
        }

        /// One linear-128 texel, for a test whose subject is not the texture.
        constexpr std::array<std::uint8_t, 4> sGreyTexel{ 128, 128, 128, 255 };

        /// The other half of de-lighting: the shader dividing the estimate back out.
        ///
        /// `ShadingMap`'s own tests say what the estimate is and `RtxShadingPassTest` says the
        /// device makes the same one; this says the frame uses it. The frame looks at the middle
        /// of the wall, `u` from 0.356 to 0.644, and the texture is bright across its middle half:
        /// the centre pixel sees a texel of 1.0 under a factor of 1.501, eight cells from either
        /// boundary, which is 0.66624 in light and `1.055 * 0.66624^(1/2.4) - 0.055` encodes to
        /// 213 of 255. Left alone it encodes to 255, a texture of one tone estimates to one
        /// everywhere and changes nothing, and the two tones flagged neutral — a composite, whose
        /// light came off in the bake — are drawn as painted under a map that was cleared and
        /// never estimated.
        TEST_F(RtxVisibilityTest, aTexturesPaintedLightIsDividedBackOutOfItsAlbedo)
        {
            constexpr std::uint32_t size = 32;
            constexpr std::size_t centre = centreValueOf(size);
            const Testing::TestTexture twoTones = Testing::paintTwoTones(32, 96);
            const Testing::TestTexture oneTone = Testing::paintTwoTones(0, 128);
            Testing::TestTexture composite = Testing::paintTwoTones(32, 96);
            composite.mData.mNeutralShading = true;

            SceneDesc scene;
            const Index mesh
                = scene.addMesh(MeshArrays{ .mPositions = sWallQuad, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
            const Index material = scene.addMaterial(
                Material{ .mDiffuse = scene.textures().add(VFS::Path::NormalizedView("tones.dds")) });
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;

            const auto shownAt = [&](float delight, const TextureData& texture) {
                camera.mDelight = delight;

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, std::span(&texture, 1), camera, size, pixels), size * size);
                return static_cast<int>(pixels[centre]);
            };

            EXPECT_NEAR(shownAt(1.0f, twoTones.mData), 213, 1) << "a texture painted half again as bright comes back";
            EXPECT_NEAR(shownAt(1.0f, oneTone.mData), 255, 1) << "and a neutral map changes nothing";
            EXPECT_NEAR(shownAt(1.0f, composite.mData), 255, 1) << "a texture not to be estimated is not";

            // The strength is what makes this answerable rather than believable: the same map at no
            // strength has to leave the texture exactly as it was drawn.
            EXPECT_NEAR(shownAt(0.0f, twoTones.mData), 255, 1) << "at zero strength the estimate is not applied";
        }

        /// The map read where the hit lands, blended and wrapped as the host reads it.
        ///
        /// **The device's read against the host's, pixel by pixel.** The texture is bright over its
        /// left half and dark over its right, so its estimate steps down at `u = 0.5` and back up at
        /// the wrap, each step blurred over six cells; between two cells the answer is a blend, and
        /// at the frame's first pixel it wraps: a wall scaled to fill the frame exactly puts that
        /// pixel's `u` at a hundred and twenty-eighth, which is a quarter of a cell before the first
        /// cell's centre, so the read leans on the last cell of the row. `Rtx::paintedLight` is the
        /// host's spelling and the composite bake reads through it, so the two agreeing is what
        /// keeps a flattened chunk and its live stack the same ground. The pixels asked are away
        /// from the two tone boundaries, where the sampled texel is one tone whole.
        ///
        /// Within a byte, which is where the map's own step and the sampler's eight-bit weights both
        /// land: a step of the device's estimate against the host's is a part in forty thousand.
        TEST_F(RtxVisibilityTest, aTexturesPaintedLightIsReadWhereTheHitLandsAsTheHostReadsIt)
        {
            constexpr std::uint32_t size = 64;
            const Testing::TestTexture painted = Testing::paintTwoTones(0, 64);
            const ShadingMap host(painted.mData);

            // The wall is four hundred across and the frame sees `2 * tan(30) * 100` of it, so this
            // scale puts the wall's edges on the frame's and `u` at `(x + 0.5) / size` at pixel `x`.
            constexpr float fills = 115.470054f / 400.0f;

            SceneDesc scene;
            const Index mesh
                = scene.addMesh(MeshArrays{ .mPositions = sWallQuad, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
            const Index material = scene.addMaterial(
                Material{ .mDiffuse = scene.textures().add(VFS::Path::NormalizedView("tones.dds")) });
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::scale(fills, 1.0f, fills), .mMesh = mesh, .mMaterial = material });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;
            camera.mDelight = 1.0f;

            std::vector<std::uint8_t> pixels;
            ASSERT_EQ(countHits(scene, std::span(&painted.mData, 1), camera, size, pixels), size * size);

            constexpr std::uint32_t row = size / 2;
            for (const std::uint32_t x : { 0u, 1u, 2u, 28u, 36u, size - 1 })
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float v = (static_cast<float>(row) + 0.5f) / static_cast<float>(size);
                const float factor = paintedLight(host.getValues(), u, v);
                const float texel = u < 0.5f ? 1.0f : 0.33247f;

                const int expected = encodeSrgb(texel / factor);
                EXPECT_NEAR(int{ pixels[(std::size_t{ row } * size + x) * 4] }, expected, 1)
                    << "at pixel " << x << ", where the host reads " << factor;
            }
        }

        /// The vertex colour a hit lands on, and what the content's mode says it replaces.
        ///
        /// **The tint replaces the material's own colour rather than multiplying it**, which is
        /// what `glColorMaterial(GL_AMBIENT_AND_DIFFUSE)` does and what the game's own shader reads
        /// through `getDiffuseColor`.
        ///
        /// The texture is a linear 128, which is 0.50196 and encodes to 188. A quad whose four
        /// vertices carry one colour interpolates to that colour everywhere, so the arithmetic is
        /// one multiply a channel: 0.5, 1 and 0.25 of 0.50196 are 0.25098, 0.50196 and 0.12549,
        /// which encode to 137, 188 and 99.
        TEST_F(RtxVisibilityTest, aVertexColourTintsTheAlbedoWhereTheContentAsksAndNowhereElse)
        {
            constexpr std::uint32_t size = 32;
            constexpr std::size_t centre = centreValueOf(size);
            const TextureData grey = describeTexel(sGreyTexel);

            const std::array<osg::Vec3f, 4> tint{ osg::Vec3f(0.5f, 1.0f, 0.25f), osg::Vec3f(0.5f, 1.0f, 0.25f),
                osg::Vec3f(0.5f, 1.0f, 0.25f), osg::Vec3f(0.5f, 1.0f, 0.25f) };

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;
            camera.mDelight = 0.0f;

            const auto albedoUnder = [&](VertexColour mode, std::span<const osg::Vec3f> colours) {
                SceneDesc scene;
                const Index mesh = scene.addMesh(MeshArrays{
                    .mPositions = sWallQuad, .mTexCoords = sQuadUv, .mColours = colours, .mIndices = sQuadIndices });
                const Index material = scene.addMaterial(Material{
                    .mDiffuse = scene.textures().add(VFS::Path::NormalizedView("grey.dds")), .mVertexColour = mode });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, std::span(&grey, 1), camera, size, pixels), size * size);

                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            const std::array<int, 3> plain{ 188, 188, 188 };
            EXPECT_EQ(albedoUnder(VertexColour::None, tint), plain) << "the content said the colours mean nothing";
            EXPECT_EQ(albedoUnder(VertexColour::Tint, tint), (std::array<int, 3>{ 137, 188, 99 }));
            EXPECT_EQ(albedoUnder(VertexColour::Glow, tint), plain) << "a glow is not a tint";

            // A mesh that brought no colour is white in the shared buffer, so the tint the shader
            // applies to it is the one that changes nothing.
            EXPECT_EQ(albedoUnder(VertexColour::Tint, {}), plain);
        }

        /// The glow the mode names is the material's own glow said another way.
        ///
        /// **Two frames rather than a figure**, because what a glow is worth depends on the
        /// ambient, the sky and `EMISSIVE_INTENSITY` — and none of that is what this is about. A
        /// material carrying the colour and a mesh carrying it are the same surface, so the two
        /// renders are the same frame.
        TEST_F(RtxVisibilityTest, aVertexGlowIsTheMaterialsOwnEmissiveColourSaidPerVertex)
        {
            constexpr std::uint32_t size = 32;
            const osg::Vec3f glow(0.75f, 0.5f, 0.25f);
            const TextureData grey = describeTexel(sGreyTexel);

            const std::array<osg::Vec3f, 4> colours{ glow, glow, glow, glow };

            const Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            const auto render = [&](VertexColour mode, const osg::Vec3f& emissive, std::vector<std::uint8_t>& pixels) {
                SceneDesc scene;
                const Index mesh = scene.addMesh(MeshArrays{
                    .mPositions = sWallQuad, .mTexCoords = sQuadUv, .mColours = colours, .mIndices = sQuadIndices });
                const Index material = scene.addMaterial(
                    Material{ .mDiffuse = scene.textures().add(VFS::Path::NormalizedView("grey.dds")),
                        .mEmissiveColour = emissive,
                        .mVertexColour = mode });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

                EXPECT_EQ(countHits(scene, std::span(&grey, 1), camera, size, pixels), size * size);
            };

            std::vector<std::uint8_t> stated;
            std::vector<std::uint8_t> perVertex;
            std::vector<std::uint8_t> unlit;
            render(VertexColour::None, glow, stated);
            render(VertexColour::Glow, osg::Vec3f(), perVertex);
            render(VertexColour::None, osg::Vec3f(), unlit);

            EXPECT_EQ(perVertex, stated) << "the vertex colour stands in for the material's own";
            EXPECT_NE(perVertex, unlit) << "and a surface that glows is not the surface that does not";
        }

        /// A four-texel sheet with a different colour in each quadrant, for the two tests that
        /// ask which part of a map a hit read: red at (0, 0), green at (1, 0), blue at (0, 1) and
        /// white at (1, 1), in texel rows, row nought first.
        constexpr std::array<std::uint8_t, 16> sQuadrants{
            255, 0, 0, 255, // (0, 0)
            0, 255, 0, 255, // (1, 0)
            0, 0, 255, 255, // (0, 1)
            255, 255, 255, 255, // (1, 1)
        };

        /// The environment sheet is added past the albedo, indexed by the eye's own reflection
        /// off the surface: `objects.vert`'s sphere map, in the frame camera's basis.
        ///
        /// **The centre pixel is exact and the corners say which way the sheet is turned.** A ray
        /// straight down the axis reflects straight back, which lands on the middle of the sheet —
        /// the bilinear mean of all four quadrants, `(0.5, 0.5, 0.5)` — so what the sheet adds
        /// there is `EMISSIVE_INTENSITY * 0.5 * tint`, which under a tint of `(1, 0.5, 0.25)` is
        /// `(4, 2, 1)`. A ray up and to the right of the axis reflects up and to the right, which
        /// lands past the middle in `u` and in `v`: more of the second column than a ray up and to
        /// the left reads, which is more green, and more of the second row than a ray down and to
        /// the left reads, which is more blue. Under the tint every channel keeps its sign.
        TEST_F(RtxVisibilityTest, anEnvironmentSheetIsAddedPastTheAlbedoWhereTheEyesReflectionLands)
        {
            // Odd, so the centre pixel's own centre is on the axis and the reflection lands on the
            // middle of the sheet exactly; at an even size it is half a pixel off and reads a
            // sixtieth more of one quadrant than the others.
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            const TextureData grey = describeTexel(sGreyTexel, 0);
            TestTexture sheet;
            paintFlat(sheet, 2, sQuadrants, "sheet");
            sheet.mData.mSlot = 1;
            const std::array<TextureData, 2> textures{ grey, sheet.mData };

            const osg::Vec3f tint(1.0f, 0.5f, 0.25f);
            const Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            const auto render = [&](bool sheeted, std::vector<float>& radiance) {
                SceneDesc scene;
                const Index mesh = scene.addMesh(
                    MeshArrays{ .mPositions = sWallQuad, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
                const Index diffuse = scene.textures().add(VFS::Path::NormalizedView("grey.dds"));
                const Index environment = scene.textures().add(VFS::Path::NormalizedView("sheet.dds"));
                const Index material = scene.addMaterial(Material{
                    .mDiffuse = diffuse,
                    .mEnvironment = sheeted ? environment : sNoIndex,
                    .mEnvironmentColour = tint,
                });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, textures, camera, size, pixels), size * size);
                radiance = mRadiance;
            };

            std::vector<float> with;
            std::vector<float> without;
            render(true, with);
            render(false, without);

            const auto addedAt = [&](std::size_t pixel) {
                return osg::Vec3f(with[pixel * 4] - without[pixel * 4], with[pixel * 4 + 1] - without[pixel * 4 + 1],
                    with[pixel * 4 + 2] - without[pixel * 4 + 2]);
            };

            const osg::Vec3f middle = addedAt(centre / 4);
            EXPECT_NEAR(middle.x(), Shaders::EMISSIVE_INTENSITY * 0.5f, 1.0e-3f);
            EXPECT_NEAR(middle.y(), Shaders::EMISSIVE_INTENSITY * 0.25f, 1.0e-3f);
            EXPECT_NEAR(middle.z(), Shaders::EMISSIVE_INTENSITY * 0.125f, 1.0e-3f);

            const osg::Vec3f upRight = addedAt(std::size_t{ 3 } * size + 29);
            const osg::Vec3f upLeft = addedAt(std::size_t{ 3 } * size + 3);
            const osg::Vec3f downLeft = addedAt(std::size_t{ 29 } * size + 3);
            EXPECT_GT(upRight.y(), upLeft.y()) << "a reflection to the right reads the sheet's second column";
            EXPECT_GT(upLeft.z(), downLeft.z()) << "a reflection upward reads the sheet's second row";
        }

        /// The dark map multiplies the albedo, read at the unit the content bound it at and on the
        /// set of texture coordinates that unit reads.
        ///
        /// **The same numbers as the tint test, arrived at by a texture.** A dark texel of
        /// `(128, 255, 64)` over the linear-128 grey is `(0.25, 0.5, 0.125)` in linear light, which
        /// the display curve encodes to `(137, 188, 99)` — the tint test's own bytes, since a tint
        /// of `(0.5, 1, 0.25)` is the same multiply. On the second set every vertex sits on the
        /// sheet's red quadrant, so a dark map bound at a unit that reads the second set darkens
        /// the wall to red; bound at unit nought it reads the first set, which spans the sheet,
        /// and the centre pixel reads the bilinear middle of all four quadrants.
        TEST_F(RtxVisibilityTest, theDarkMapMultipliesTheAlbedoOnTheSetItsUnitReads)
        {
            // Odd for the reason the sheet test gives: the centre pixel reads the sheet's exact
            // middle.
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            const TextureData grey = describeTexel(sGreyTexel, 0);
            constexpr std::array<std::uint8_t, 4> sDarkTexel{ 128, 255, 64, 255 };
            const TextureData dark = describeTexel(sDarkTexel, 1);
            TestTexture sheet;
            paintFlat(sheet, 2, sQuadrants, "sheet");
            sheet.mData.mSlot = 2;
            const std::array<TextureData, 3> textures{ grey, dark, sheet.mData };

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;
            camera.mDelight = 0.0f;

            const std::array<osg::Vec2f, 4> onRed{ osg::Vec2f(0.25f, 0.25f), osg::Vec2f(0.25f, 0.25f),
                osg::Vec2f(0.25f, 0.25f), osg::Vec2f(0.25f, 0.25f) };

            const auto albedoUnder = [&](std::uint32_t darkSlot, std::uint8_t unit, std::uint32_t unitStreams) {
                SceneDesc scene;
                const Index mesh = scene.addMesh(MeshArrays{
                    .mPositions = sWallQuad,
                    .mTexCoords = sQuadUv,
                    .mSecondTexCoords = onRed,
                    .mUnitStreams = unitStreams,
                    .mIndices = sQuadIndices,
                });
                const Index diffuse = scene.textures().add(VFS::Path::NormalizedView("grey.dds"));
                const Index darkFlat = scene.textures().add(VFS::Path::NormalizedView("dark.dds"));
                const Index darkSheet = scene.textures().add(VFS::Path::NormalizedView("sheet.dds"));
                EXPECT_EQ(darkFlat, 1u);
                EXPECT_EQ(darkSheet, 2u);
                const Index material = scene.addMaterial(Material{
                    .mDiffuse = diffuse,
                    .mDark = darkSlot,
                    .mDarkUnit = unit,
                });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, textures, camera, size, pixels), size * size);
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            EXPECT_EQ(albedoUnder(sNoIndex, 0, 0), (std::array<int, 3>{ 188, 188, 188 }));
            EXPECT_EQ(albedoUnder(1, 1, 0), (std::array<int, 3>{ 137, 188, 99 }));

            // The sheet on the first set: the centre pixel reads the middle of it, a half in every
            // channel, which halves the grey to the tint test's 137.
            EXPECT_EQ(albedoUnder(2, 0, 0), (std::array<int, 3>{ 137, 137, 137 }));

            // The sheet on the second set, at a unit that reads it: red everywhere.
            EXPECT_EQ(albedoUnder(2, 1, 1u << 1), (std::array<int, 3>{ 188, 0, 0 }));

            // And at a unit the mesh says reads the first set, the second set is not read.
            EXPECT_EQ(albedoUnder(2, 1, 0), (std::array<int, 3>{ 137, 137, 137 }));
        }

        /// A surface that adds is met by no ray that shades and adds at the picture's own extent.
        ///
        /// **Two questions of one scene.** The trace's own frame must not change when an additive
        /// quad is held in front of the wall — no shading ray meets it, so the wall behind is lit
        /// and hit exactly as before — and the shown picture must be brighter where the quad is and
        /// unchanged where it is not, because the composite gathered it there and nowhere else.
        TEST_F(RtxVisibilityTest, anAdditiveSurfaceAddsToThePictureAndIsMetByNothingThatShades)
        {
            constexpr std::uint32_t size = 32;
            constexpr std::size_t centre = centreValueOf(size);

            const TextureData grey = describeTexel(sGreyTexel, 0);
            constexpr std::array<std::uint8_t, 4> sRedTexel{ 255, 0, 0, 255 };
            const TextureData red = describeTexel(sRedTexel, 1);
            const std::array<TextureData, 2> textures{ grey, red };

            const Shaders::VisibilityConstants camera = wallCamera(
                size, osg::Vec3f(2.0f, 2.0f, 2.0f), osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f));

            // A quad forty units across held halfway to the wall, which the centre pixel looks
            // through and a corner pixel does not.
            const std::array<osg::Vec3f, 4> held = uprightQuadAt(20.0f, -50.0f);

            const auto build = [&](bool withQuad) {
                SceneDesc scene;
                const Index wall = scene.addMesh(
                    MeshArrays{ .mPositions = sWallQuad, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
                const Index diffuse = scene.textures().add(VFS::Path::NormalizedView("grey.dds"));
                const Index glow = scene.textures().add(VFS::Path::NormalizedView("red.dds"));
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = wall,
                    .mMaterial = scene.addMaterial(Material{ .mDiffuse = diffuse }) });

                if (withQuad)
                {
                    const Index sheet = scene.addMesh(
                        MeshArrays{ .mPositions = held, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
                    const Index additive = scene.addMaterial(Material{
                        .mDiffuse = glow,
                        .mOpacity = 0.5f,
                        .mAlphaMode = AlphaMode::Blend,
                        .mBlend = BlendKind::Add,
                    });
                    scene.addInstance(
                        MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = sheet, .mMaterial = additive });
                }

                return scene;
            };

            // The trace's own depth and bounce, read before the composite that gathers what adds:
            // a quad the eye met would stand at fifty where the wall stands at a hundred.
            std::vector<float> withDepth;
            std::vector<float> withoutDepth;
            std::vector<float> withBounce;
            std::vector<float> withoutBounce;
            std::vector<std::uint8_t> withShown;
            std::vector<std::uint8_t> withoutShown;

            EXPECT_EQ(renderShot(build(true), textures, camera, size), size * size);
            mRenderer->readChannel(Channel::Depth, withDepth);
            mRenderer->readChannel(Channel::Indirect, withBounce);
            mRenderer->readPixels(withShown);

            EXPECT_EQ(renderShot(build(false), textures, camera, size), size * size);
            mRenderer->readChannel(Channel::Depth, withoutDepth);
            mRenderer->readChannel(Channel::Indirect, withoutBounce);
            mRenderer->readPixels(withoutShown);

            requireFrame(withShown, size);
            requireFrame(withoutShown, size);

            // The depth channel is two values a pixel: the clip depth, and the distance the ray
            // travelled.
            ASSERT_EQ(withDepth.size(), std::size_t{ size } * size * 2);
            EXPECT_NEAR(withDepth[centreOf(size) * 2 + 1], 100.0f, 0.1f) << "the eye met the additive quad";
            EXPECT_EQ(withDepth, withoutDepth);
            EXPECT_EQ(withBounce, withoutBounce) << "a bounce met the additive quad";

            EXPECT_GT(int{ withShown[centre] }, int{ withoutShown[centre] }) << "the quad added nothing red";
            EXPECT_EQ(int{ withShown[centre + 1] }, int{ withoutShown[centre + 1] }) << "and nothing green";

            const std::size_t corner = 4;
            EXPECT_EQ(withShown[corner], withoutShown[corner]) << "the quad reached a pixel it does not cover";
            EXPECT_EQ(withShown[corner + 1], withoutShown[corner + 1]);
        }

        /// The mip chain a ray cone selects from, at a distance chosen so the answer is a whole
        /// number.
        ///
        /// Sixty-four texels across a quad that exactly fills a sixty-degree frame at a hundred
        /// units: one texel per pixel, so the cone is one texel wide and the level is zero. The
        /// arithmetic, which the shader repeats:
        ///
        ///   spread    = atan(2 * tan(30) / 64)  = 0.0180402 radians per pixel
        ///   coneWidth = spread * 100            = 1.80402 units
        ///   texelArea = 1 * 64 * 64             = 4096      (the shader's doubled form)
        ///   worldArea = |cross| = 115.47^2      = 13333.3   (doubled the same way, so it cancels)
        ///   lambda    = 0.5 * log2(4096/13333.3) + log2(1.80402) = -0.85138 + 0.85140 = 0
        ///
        /// Double the distance and the cone doubles, so lambda becomes exactly one. Each level is a
        /// different grey, so the level chosen is legible in a single pixel.
        TEST_F(RtxVisibilityTest, theConeReadsTheMipTheDistanceCallsFor)
        {
            constexpr std::uint32_t size = 64;

            TestTexture ladder;
            paintMipLadder(ladder);
            const std::span<const TextureData> textures(&ladder.mData, 1);

            const std::array positions = cardAt(0.0f);

            SceneDesc scene;
            const Index mesh
                = scene.addMesh(MeshArrays{ .mPositions = positions, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
            const Index material
                = scene.addMaterial(Material{ .mDiffuse = scene.textures().add(VFS::Path::NormalizedView("mip.dds")) });
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

            const auto centreOf = [](const std::vector<std::uint8_t>& pixels) { return pixels[centreValueOf(size)]; };

            const auto renderAt = [&](float distance) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -distance, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mShowAlbedo = 1u;

                std::vector<std::uint8_t> pixels;
                countHits(scene, textures, camera, size, pixels);
                return centreOf(pixels);
            };

            // Within a byte, because the claim is which level was read and the levels are twenty-odd
            // bytes apart once encoded — no rounding difference between the shader's transfer
            // function and this one can make a level look like its neighbour. Exact equality would
            // fail on the third, whose encoded value happens to land on 207.51.
            EXPECT_NEAR(renderAt(100.0f), encodeSrgb(40.0f / 255.0f), 1);
            EXPECT_NEAR(renderAt(200.0f), encodeSrgb(70.0f / 255.0f), 1);

            // And the far end of the ladder, so a shader that clamped at level one would be caught.
            EXPECT_NEAR(renderAt(1600.0f), encodeSrgb(160.0f / 255.0f), 1);
        }

        /// Ground: layers summed by their masks, at the weights the mask grid names.
        ///
        /// Two layers over one quad, each a solid colour, with a mask two weights wide: layer zero
        /// is [1, 0] and layer one [0, 1]. A mask samples at `u * width - 0.5`, so texel centres sit
        /// at u = 0.25 and u = 0.75 and the weight between them is a straight ramp — pure layer zero
        /// left of the first centre, pure layer one right of the second, and exactly half of each in
        /// the middle. Those three points are what this checks, because they are the ones the
        /// arithmetic pins: 0.5 of a linear one encodes to 1.055 * 0.5^(1/2.4) - 0.055, or 188.
        TEST_F(RtxVisibilityTest, groundSumsItsLayersByTheWeightsItsMasksName)
        {
            constexpr std::uint32_t size = 64;

            // Two solid textures and one two-texel strip, all one level so nothing but the layer
            // arithmetic can move a byte.
            const auto makeSolid = [](std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
                return std::array<std::uint8_t, 4>{ red, green, blue, 255 };
            };
            const std::array<std::uint8_t, 4> redTexel = makeSolid(255, 0, 0);
            const std::array<std::uint8_t, 4> greenTexel = makeSolid(0, 255, 0);
            // Sixty-four texels for sixty-four columns, so every pixel samples exactly one texel
            // centre and no filtering weight can enter the answer. Green for the first half, blue
            // for the second.
            std::array<std::uint8_t, size * 4> strip{};
            for (std::uint32_t texel = 0; texel < size; ++texel)
                strip[texel * 4 + (texel < size / 2 ? 1 : 2)] = 255;

            const MipLevel wide{ 0, size, 1 };
            const std::array<TextureData, 3> textures{
                describeTexel(redTexel),
                describeTexel(greenTexel),
                TextureData{
                    .mFormat = TextureFormat::Rgba8Unorm,
                    .mWidth = size,
                    .mHeight = 1,
                    .mBytes = std::as_bytes(std::span(strip)),
                    .mLevels = std::span(&wide, 1),
                },
            };

            const std::array positions = cardAt(0.0f);
            constexpr std::array<float, 2> firstMask{ 1.0f, 0.0f };
            constexpr std::array<float, 2> secondMask{ 0.0f, 1.0f };

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;

            /// @param second the texture slot and diffuse transform of the layer on the right.
            const auto render = [&](Index second, const osg::Vec4f& secondTransform) {
                SceneDesc scene;
                const Index mesh = scene.addMesh(
                    MeshArrays{ .mPositions = positions, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
                scene.textures().add(VFS::Path::NormalizedView("red.dds"));
                scene.textures().add(VFS::Path::NormalizedView("green.dds"));
                scene.textures().add(VFS::Path::NormalizedView("strip.dds"));

                const std::array layers{
                    MaterialLayer{
                        .mDiffuse = 0,
                        .mMask = scene.materials().addMask(firstMask),
                        .mPlacing = { .mMaskWidth = 2, .mMaskHeight = 1 },
                    },
                    MaterialLayer{
                        .mDiffuse = second,
                        .mMask = scene.materials().addMask(secondMask),
                        .mPlacing = { .mMaskWidth = 2, .mMaskHeight = 1, .mDiffuseTransform = secondTransform },
                    },
                };
                const Rtx::Run run = scene.materials().addLayers(layers);

                Material material;
                material.mKind = MaterialKind::Terrain;
                material.mLayers = run;

                scene.addInstance(MeshInstance{
                    .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = scene.addMaterial(material) });

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, textures, camera, size, pixels), size * size);
                return pixels;
            };

            const std::vector<std::uint8_t> ramp = render(1, osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f));

            // Column c samples u = (c + 0.5) / 64, so the two texel centres fall on columns 15.5 and
            // 47.5 and the middle of the ramp on column 31.5. Sampling either side of a boundary
            // would land on a value the ramp only reaches between pixels.
            const auto at = [&](const std::vector<std::uint8_t>& pixels, std::uint32_t column) {
                return &pixels[(std::size_t{ size / 2 } * size + column) * 4];
            };

            EXPECT_EQ(at(ramp, 0)[0], 255) << "pure first layer, red";
            EXPECT_EQ(at(ramp, 0)[1], 0);
            EXPECT_EQ(at(ramp, 63)[0], 0) << "pure second layer, green";
            EXPECT_EQ(at(ramp, 63)[1], 255);

            // Columns 31 and 32 straddle the halfway point by half a pixel each, so neither is an
            // even split and the two are mirror images. Column 31 samples u = 31.5 / 64 = 0.49219,
            // which is 0.48438 of the way from the first texel centre to the second, so the second
            // layer weighs that and the first weighs 0.51563. Encoded:
            //
            //   1.055 * 0.51563^(1/2.4) - 0.055 = 0.74547, or 190 of 255
            //   1.055 * 0.48438^(1/2.4) - 0.055 = 0.72503, or 185 of 255
            EXPECT_EQ(at(ramp, 31)[0], 190) << "the first layer, three sixty-fourths past centre";
            EXPECT_EQ(at(ramp, 31)[1], 185);
            EXPECT_EQ(at(ramp, 32)[0], 185) << "and the mirror of it on the other side";
            EXPECT_EQ(at(ramp, 32)[1], 190);

            // Outside the two centres the ramp is flat, which is the clamp doing its work: a mask
            // that wrapped would fold the far layer back over the near one at both edges.
            EXPECT_EQ(at(ramp, 15)[0], 255) << "still pure at the first texel centre";
            EXPECT_EQ(at(ramp, 48)[1], 255) << "and at the second";

            // The layer's own texture transform, proved by moving it under a fixed pixel. Column 63
            // samples u = 0.99219, which on the sixty-four-texel strip is texel 63's centre — the
            // blue half. Half a unit of offset puts the same pixel on texel 31, the green half, and
            // both are exact centres so the answer is a texel rather than a blend of two.
            const std::vector<std::uint8_t> blue = render(2, osg::Vec4f(1.0f, 1.0f, 0.0f, 0.0f));
            const std::vector<std::uint8_t> green = render(2, osg::Vec4f(1.0f, 1.0f, -0.5f, 0.0f));

            EXPECT_EQ(at(blue, 63)[2], 255) << "the strip's far half";
            EXPECT_EQ(at(blue, 63)[1], 0);
            EXPECT_EQ(at(green, 63)[1], 255) << "and its near half, half a coordinate back";
            EXPECT_EQ(at(green, 63)[2], 0);
        }

        /// A chunk flattened on the device is the ground its stack sums, whichever way it arrived.
        ///
        /// Two solid layers at constant weights, a quarter of red and three quarters of green, so
        /// the composite is one colour at every level and the trace's choice of level cannot
        /// enter: the stack gives (0.25, 0.75) linear, encoded 1.055 * 0.25^(1/2.4) - 0.055 =
        /// 0.53711, or 137, and 1.055 * 0.75^(1/2.4) - 0.055 = 0.88083, or 225. What the bake
        /// sums, and at which level, is `RtxGroundCompositePassTest`'s; this is that a chunk
        /// given its slot draws the composite and not its stack, by both roads a composite
        /// arrives on — a world built from nothing, whose bake is the build's own batch, and an
        /// arrival into a standing world, whose bake is the placement after it — a placement that
        /// records nothing else, because the world stands still, and is submitted for the bake.
        TEST_F(RtxVisibilityTest, aFlattenedChunkDrawsItsCompositeHoweverItArrived)
        {
            constexpr std::uint32_t size = 64;

            const std::array<std::uint8_t, 4> redTexel{ 255, 0, 0, 255 };
            const std::array<std::uint8_t, 4> greenTexel{ 0, 255, 0, 255 };
            const std::array positions = cardAt(0.0f);
            constexpr std::array<float, 1> quarter{ 0.25f };
            constexpr std::array<float, 1> threeQuarters{ 0.75f };

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;

            SceneDesc scene;
            const Index mesh
                = scene.addMesh(MeshArrays{ .mPositions = positions, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
            scene.textures().add(VFS::Path::NormalizedView("red.dds"));
            scene.textures().add(VFS::Path::NormalizedView("green.dds"));

            const std::array layers{
                MaterialLayer{
                    .mDiffuse = 0,
                    .mMask = scene.materials().addMask(quarter),
                    .mPlacing = { .mMaskWidth = 1, .mMaskHeight = 1 },
                },
                MaterialLayer{
                    .mDiffuse = 1,
                    .mMask = scene.materials().addMask(threeQuarters),
                    .mPlacing = { .mMaskWidth = 1, .mMaskHeight = 1 },
                },
            };

            Material material;
            material.mKind = MaterialKind::Terrain;
            material.mLayers = scene.materials().addLayers(layers);
            const Index chunk = scene.addMaterial(material);
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = chunk });

            const auto everyPixelIs = [&](const std::vector<std::uint8_t>& pixels, const char* road) {
                ASSERT_EQ(pixels.size(), std::size_t{ size } * size * 4) << road;
                for (std::size_t at = 0; at < pixels.size(); at += 4 * 61)
                {
                    EXPECT_NEAR(int{ pixels[at] }, 137, 1) << road << " at pixel " << at / 4;
                    EXPECT_NEAR(int{ pixels[at + 1] }, 225, 1) << road << " at pixel " << at / 4;
                    EXPECT_EQ(int{ pixels[at + 2] }, 0) << road << " at pixel " << at / 4;
                }
            };

            const std::array<TextureData, 2> stackTextures{ describeTexel(redTexel), describeTexel(greenTexel) };
            std::vector<std::uint8_t> pixels;
            EXPECT_EQ(countHits(scene, stackTextures, camera, size, pixels), size * size);
            everyPixelIs(pixels, "the stack");

            // Two more frames with the placement ended after each, as the uploader ends it, so
            // both copies of the tables are written and owe nothing — the instance settles into
            // the other copy on the first, and that write is owed back to the first copy on the
            // second. The placement below then owes nothing but the bake: a world that stands
            // still records no refit and no top level, and the bake has to be reason enough to
            // submit it.
            for (int frame = 0; frame < 2; ++frame)
            {
                scene.placements().advance();
                mRenderer->placeScene(Rtx::SceneSlot::world(), scene);
                mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });
                ASSERT_TRUE(mRenderer->finishFrame().has_value());
            }
            scene.placements().advance();

            // The chunk asks and is given its slot, as `CompositeQueue::advance` gives it: the
            // material rewritten in place, and the slot described as the builder describes a
            // composite — which chunk it is the ground of, and no bytes.
            Material flattened = material;
            flattened.mFlatten = true;
            flattened.mDiffuse = scene.textures().addBaked("chunk/0");
            scene.setMaterial(chunk, flattened);
            const TextureData composite{
                .mSlot = flattened.mDiffuse,
                .mFormat = TextureFormat::Rgba8Srgb,
                .mCompositeOf = chunk,
                .mNeutralShading = true,
            };

            // Into the standing world: the arrival stands the composite empty, and the placement
            // `extendScene` ends in bakes it.
            mRenderer->extendScene(Rtx::SceneSlot::world(), scene, std::span(&composite, 1));
            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });
            ASSERT_TRUE(mRenderer->finishFrame().has_value());
            encodeLastFrame(size, pixels);
            everyPixelIs(pixels, "arrived into a standing world");

            // And from nothing, where there is no placement before the first trace.
            const std::array<TextureData, 3> flattenedTextures{ describeTexel(redTexel), describeTexel(greenTexel),
                composite };
            EXPECT_EQ(countHits(scene, flattenedTextures, camera, size, pixels), size * size);
            everyPixelIs(pixels, "built from nothing");
        }
    }
}

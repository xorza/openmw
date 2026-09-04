#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <components/rtx/shadingmap.hpp>

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
            Graveyard graveyard(device, pool);
            const SceneBuffers buffers(device, empty, {}, 1, graveyard);

            // **Every table this hands out, and not the three that were caught.** The rule was the
            // same for all of them; which ones happened to be empty on the day is not what decides
            // whether they are covered. An address of nought is a table bound as nothing, and an
            // address off what its reference claims is a load the device may split or fault on, with
            // no message either.
            Shaders::GpuTables addressed{};
            buffers.describeTables(0, addressed);

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
            const Index mesh = scene.addMesh(sWallQuad, {}, sQuadUv, sQuadIndices);
            const Index red
                = scene.addMaterial(Material{ .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("red.dds")) });
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = red });

            mRenderer->resize(size, size);
            const TextureData first = describeTexel(redTexel, 0);
            mRenderer->setScene(Rtx::sWorld, scene, std::span(&first, 1), SeaState{});
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
            ASSERT_EQ(mRenderer->getTextureCount(Rtx::sWorld), 1u);

            // A second texture and a second material, on a wall nearer the eye. The mesh table is
            // untouched, so this is the append path and not a rebuild.
            const Index blue
                = scene.addMaterial(Material{ .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("blue.dds")) });
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::translate(0.0f, -50.0f, 0.0f), .mMesh = mesh, .mMaterial = blue });

            // **The slot the scene gave it**, which is what an arrival now carries: a texture is
            // written where it belongs rather than after whatever is already there.
            const Index blueTexture = scene.getMaterials()[blue].mDiffuse;
            const TextureData second = describeTexel(blueTexel, blueTexture);
            mRenderer->extendScene(Rtx::sWorld, scene, std::span(&second, 1), SeaState{});
            EXPECT_EQ(mRenderer->getTextureCount(Rtx::sWorld), 2u);

            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });
            mRenderer->readPixels(shown);

            // The nearer wall wears the texture that was appended, which is only true if its
            // descriptor went to element one. Written to element zero it would come out red, and
            // written nowhere it would come out as whatever the array holds there — both plausible.
            EXPECT_TRUE(wearsBlue(centre)) << "the near wall wears the texture that was appended";

            // And the first texture is still where it was: move the near wall out of the way and the
            // one behind it has to be red again, sampled from a descriptor nothing rewrote.
            scene.dropInstance(1);
            mRenderer->placeScene(Rtx::sWorld, scene, SeaState{});
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
            ASSERT_TRUE(scene.isTextureFree(blueTexture));
            ASSERT_EQ(scene.getTextures().size(), 2u) << "the table does not shrink";

            mRenderer->setScene(Rtx::sWorld, scene, std::span(&first, 1), SeaState{});

            EXPECT_EQ(mRenderer->getTextureCount(Rtx::sWorld), 2u)
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
            const Index again
                = scene.addMaterial(Material{ .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("blue.dds")) });
            ASSERT_EQ(scene.getMaterials()[again].mDiffuse, blueTexture) << "the freed slot was not taken over";

            scene.dropInstance(0);
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = again });

            const std::array<Index, 1> keptAgain{ again };
            ASSERT_TRUE(scene.release(keptMeshes, keptAgain));
            ASSERT_TRUE(scene.isTextureFree(0u));

            mRenderer->setScene(Rtx::sWorld, scene, std::span(&second, 1), SeaState{});
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
            const Index mesh = textured.addMesh(sWallQuad, {}, sQuadUv, sQuadIndices);
            const Index material
                = textured.addMaterial(Material{ .mDiffuse = textured.addTexture(VFS::Path::NormalizedView("red.dds")),
                    .mEmissive = textured.addTexture(VFS::Path::NormalizedView("green.dds")) });
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
            camera.mSunPosition = osg::Vec3f(-1.0f, -0.2f, 0.0f);
            camera.mSunIrradiance = osg::Vec3f(3.0f, 3.0f, 3.0f);

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
                const Index mesh = scene.addMesh(sWallQuad, quadNormals, sQuadUv, sQuadIndices);
                const Index material = scene.addMaterial(
                    Material{ .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("corners.dds")) });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });
                return mesh;
            };

            SceneDesc single;
            const Index alone = addWall(single);
            ASSERT_EQ(single.getMeshes()[alone].mVertexOffset, 0u);

            // **One filler that fills both blocks**, because the vertex and index tables are blocked
            // at different sizes and a mesh pushed past one is not thereby past the other.
            SceneDesc crossed;
            const std::vector<osg::Vec3f> fillerVertices(SceneDesc::sVertexBlock, osg::Vec3f(0.0f, 0.0f, 0.0f));

            // The largest whole number of triangles a block holds, so what is left of it is one
            // index and the wall's six cannot fit.
            std::vector<std::uint32_t> fillerIndices(SceneDesc::sIndexBlock / 3 * 3);
            for (std::size_t at = 0; at < fillerIndices.size(); ++at)
                fillerIndices[at] = static_cast<std::uint32_t>(at % 3);

            crossed.addMesh(fillerVertices, {}, {}, fillerIndices);
            const Index beyond = addWall(crossed);

            // Hand-computed: the filler is a whole vertex block, so the wall starts the next one;
            // and 1,048,575 of 1,048,576 indices leaves a tail of one, which six will not fit into.
            // Asserted, because a test whose subject quietly moved back into block zero would pass
            // while testing nothing.
            ASSERT_EQ(crossed.getMeshes()[beyond].mVertexOffset, SceneDesc::sVertexBlock);
            ASSERT_EQ(crossed.getMeshes()[beyond].mIndexOffset, SceneDesc::sIndexBlock);

            std::vector<std::uint8_t> alonePixels;
            std::vector<std::uint8_t> crossedPixels;
            EXPECT_EQ(countHits(single, std::span(&painted, 1), camera, size, alonePixels), size * size);
            EXPECT_EQ(countHits(crossed, std::span(&painted, 1), camera, size, crossedPixels), size * size);

            EXPECT_EQ(crossedPixels, alonePixels)
                << "the wall shaded differently once its vertices moved into the second block";
        }

        /// One linear-128 texel with `shading` painted over it: a texture with nothing in it but a
        /// map, which is what a test of the estimate's other half wants.
        constexpr std::array<std::uint8_t, 4> sGreyTexel{ 128, 128, 128, 255 };

        TextureData describeGrey(std::span<const float> shading)
        {
            TextureData grey = describeTexel(sGreyTexel);
            grey.mShading = shading;
            return grey;
        }

        /// The other half of de-lighting: the shader dividing the estimate back out.
        ///
        /// `ShadingMap`'s own tests say the estimate is right; this says the frame uses it. A map is
        /// handed in rather than estimated, so what is asserted is the arithmetic at the sample and
        /// nothing about how the number was arrived at.
        ///
        /// The texture is a linear 128, which is 0.50196. Divided by a map of two that is 0.25098,
        /// and `1.055 * 0.25098^(1/2.4) - 0.055` encodes to 137 of 255; left alone it encodes to
        /// 188, which is what a half-lit surface comes out at for the same reason.
        TEST_F(RtxVisibilityTest, aTexturesPaintedLightIsDividedBackOutOfItsAlbedo)
        {
            constexpr std::uint32_t size = 32;
            constexpr std::size_t centre = centreValueOf(size);
            std::array<float, ShadingMap::sCells> painted{};
            const TextureData grey = describeGrey(painted);

            SceneDesc scene;
            const Index mesh = scene.addMesh(sWallQuad, {}, sQuadUv, sQuadIndices);
            const Index material
                = scene.addMaterial(Material{ .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("grey.dds")) });
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;

            const auto shownAt = [&](float delight, float factor) {
                painted.fill(factor);
                camera.mDelight = delight;

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, std::span(&grey, 1), camera, size, pixels), size * size);
                return static_cast<int>(pixels[centre]);
            };

            EXPECT_NEAR(shownAt(1.0f, 2.0f), 137, 1) << "a texture painted twice as bright comes back half";
            EXPECT_NEAR(shownAt(1.0f, 1.0f), 188, 1) << "and a neutral map changes nothing";

            // The strength is what makes this answerable rather than believable: the same map at no
            // strength has to leave the texture exactly as it was drawn.
            EXPECT_NEAR(shownAt(0.0f, 2.0f), 188, 1) << "at zero strength the estimate is not applied";
        }

        /// The map read where the hit lands, blended and wrapped as the host reads it.
        ///
        /// **The device's read against the host's, pixel by pixel.** The map is a gradient along
        /// `u` from the floor to the ceiling, so between two cells the answer is a blend and at the
        /// frame's first pixel it wraps: a wall scaled to fill the frame exactly puts that pixel's
        /// `u` at a hundred and twenty-eighth, which is a quarter of a cell before the first cell's
        /// centre, so the read leans on the last cell of the row. `Rtx::paintedLight` is the host's
        /// spelling and the composite bake reads through it, so the two agreeing is what keeps a
        /// flattened chunk and its live stack the same ground.
        ///
        /// Within a byte, which is where the map's own step and the sampler's eight-bit weights both
        /// land: the gradient changes by a twentieth between neighbouring cells, and an eighth of a
        /// per cent of that is nothing a byte can see.
        TEST_F(RtxVisibilityTest, aTexturesPaintedLightIsReadWhereTheHitLandsAsTheHostReadsIt)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::uint32_t extent = ShadingMap::sExtent;

            std::array<float, ShadingMap::sCells> painted{};
            for (std::size_t row = 0; row < extent; ++row)
                for (std::size_t column = 0; column < extent; ++column)
                    painted[row * extent + column] = ShadingMap::sFloor
                        + (ShadingMap::sCeiling - ShadingMap::sFloor) * static_cast<float>(column)
                            / static_cast<float>(extent - 1);

            const TextureData grey = describeGrey(painted);

            // The wall is four hundred across and the frame sees `2 * tan(30) * 100` of it, so this
            // scale puts the wall's edges on the frame's and `u` at `(x + 0.5) / size` at pixel `x`.
            constexpr float fills = 115.470054f / 400.0f;

            SceneDesc scene;
            const Index mesh = scene.addMesh(sWallQuad, {}, sQuadUv, sQuadIndices);
            const Index material
                = scene.addMaterial(Material{ .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("grey.dds")) });
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::scale(fills, 1.0f, fills), .mMesh = mesh, .mMaterial = material });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;
            camera.mDelight = 1.0f;

            std::vector<std::uint8_t> pixels;
            ASSERT_EQ(countHits(scene, std::span(&grey, 1), camera, size, pixels), size * size);

            constexpr std::uint32_t row = size / 2;
            for (const std::uint32_t x : { 0u, 1u, size / 2, size - 1 })
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float v = (static_cast<float>(row) + 0.5f) / static_cast<float>(size);
                const float factor = paintedLight(painted, u, v);

                const int expected = encodeSrgb(128.0f / 255.0f / factor);
                EXPECT_NEAR(int{ pixels[(std::size_t{ row } * size + x) * 4] }, expected, 1)
                    << "at pixel " << x << ", where the host reads " << factor;
            }
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
            const Index mesh = scene.addMesh(positions, {}, sQuadUv, sQuadIndices);
            const Index material
                = scene.addMaterial(Material{ .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("mip.dds")) });
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
                const Index mesh = scene.addMesh(positions, {}, sQuadUv, sQuadIndices);
                scene.addTexture(VFS::Path::NormalizedView("red.dds"));
                scene.addTexture(VFS::Path::NormalizedView("green.dds"));
                scene.addTexture(VFS::Path::NormalizedView("strip.dds"));

                const std::array layers{
                    MaterialLayer{
                        .mDiffuse = 0,
                        .mMaskOffset = scene.addMask(firstMask),
                        .mMaskWidth = 2,
                        .mMaskHeight = 1,
                    },
                    MaterialLayer{
                        .mDiffuse = second,
                        .mMaskOffset = scene.addMask(secondMask),
                        .mMaskWidth = 2,
                        .mMaskHeight = 1,
                        .mDiffuseTransform = secondTransform,
                    },
                };
                const Span run = scene.addLayers(layers);

                Material material;
                material.mKind = MaterialKind::Terrain;
                material.mLayerOffset = run.mOffset;
                material.mLayerCount = run.mCount;

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
    }
}

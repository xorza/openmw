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
        /// Nothing here loads the validation layers, because they allocate and this is what counts
        /// allocations.
        ///
        /// **A device and a renderer, and the two tests take one each.** The frame is measured
        /// through `getUnvalidatedRenderer`, which the fixture does not supply; what the fixture
        /// supplies is the raw device the texture array below is built on. Both are unvalidated for
        /// the one reason, which `getUnvalidatedHarness` states with the number.
        struct RtxFrameCostTest : DeviceTest
        {
            RtxFrameCostTest()
                : DeviceTest(false)
            {
            }
        };

        /// A warm renderer draws a still frame without going to the heap once.
        ///
        /// The concern is jitter rather than throughput: at sixty frames a second a single allocator
        /// stall is a dropped frame, and an average hides it. What this forbids on the frame path is
        /// a `std::string` built, an unreserved vector grown, a `std::function` captured, a
        /// `make_unique` reached for, or logging that did not compile out.
        ///
        /// **Through `Rtx::Renderer` and not through the passes it owns.** What this replaced
        /// assembled the frame by hand — the skin pass, the structures, the trace and the composite,
        /// recorded in the order `VulkanRenderer` records them — which is a second copy of the frame
        /// path that could go on passing while the real one grew a pass that allocated. It also left
        /// out most of the frame: the fog volume's three passes, the accumulator, the wavelet, the
        /// bloom, the tone curve, the exposure, the ring the frame is drawn into and `finishFrame`.
        /// Asked of the renderer, all of it is measured and none of it can drift.
        ///
        /// **Four shapes, because they light different code.** A plain frame traces and composites; a
        /// filtered one adds the wavelet's five levels and its history; an accumulating one adds the
        /// sum image; and a body walking is the one thing the frame path *computes* rather than
        /// copies — a pose the host writes, a dispatch, a refit, and a rebuild of what moved. The
        /// camera stands still through all four, which is what pins them: a moving camera would
        /// allocate nothing either, and then nothing would be pinned.
        ///
        /// **Warmed first, because the first of anything legitimately allocates**: descriptor pools
        /// grow, the driver caches its first call, and a command buffer finds its size.
        TEST_F(RtxFrameCostTest, aWarmRendererDrawsAStillFrameWithoutTheHeap)
        {
            constexpr std::uint32_t size = 64;
            constexpr int warmUpFrames = 16;
            constexpr int measuredFrames = 32;

            // What a frame is allowed, and why. Zero is the claim; a constant rather than a literal
            // zero is so that a path which must allocate can be admitted deliberately, with the
            // number and the reason written down beside it.
            constexpr std::size_t budgetPerFrame = 0;

            std::string reason;
            Renderer* renderer = getUnvalidatedRenderer(reason);
            if (renderer == nullptr)
                GTEST_SKIP() << reason;

            // A wall to trace, and a second one behind it skinned to one bone so that there is a body
            // to pose.
            SceneDesc scene = makeWall();
            const Index body = scene.addMesh(sWallQuad, {}, {}, sQuadIndices, {}, Deform::Rig, addOneBoneRig(scene, 4));
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::translate(0.0f, 100.0f, 0.0f), .mMesh = body });
            poseByOneBone(scene, body, osg::Matrixf::identity());

            renderer->resize(size, size);
            renderer->setScene(Rtx::sWorld, scene, {}, SeaState{});

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mSunIrradiance = osg::Vec3f(2.0f, 2.0f, 2.0f);

            // The sampler's own frame, which advances across every leg: two legs handed the same one
            // would be two legs drawing one picture, and a renderer that noticed could skip work this
            // is measuring.
            std::uint32_t index = 0;
            float walked = 0.0f;

            const auto measure = [&](const char* what, const FrameOptions& options, bool moving) {
                const auto frame = [&] {
                    if (moving)
                    {
                        // What the game's walk does to a body every frame: a new pose, placed.
                        walked += 1.0f;
                        scene.clearPlacement();
                        poseByOneBone(scene, body, osg::Matrixf::translate(0.0f, walked, 0.0f));
                        renderer->placeScene(Rtx::sWorld, scene, SeaState{});
                    }

                    camera.mFrame = index++;
                    renderer->renderFrame(camera, options);
                    renderer->finishFrame();
                };

                for (int i = 0; i < warmUpFrames; ++i)
                    frame();

                const std::size_t before = Testing::getAllocationCount();
                for (int i = 0; i < measuredFrames; ++i)
                    frame();
                const std::size_t after = Testing::getAllocationCount();

                EXPECT_LE(after - before, budgetPerFrame * measuredFrames)
                    << what << ": " << (after - before) << " allocations across " << measuredFrames << " frames";
            };

            measure("a plain frame", FrameOptions{ .mFilter = false }, false);
            measure("a filtered frame", FrameOptions{ .mFilter = true }, false);
            measure("an accumulating frame", FrameOptions{ .mAccumulate = 1, .mFilter = true }, false);
            measure("a body walking", FrameOptions{ .mFilter = true }, true);

            // **And the read back, which the harness does every frame and the window does never.**
            // Warmed by one call, because the first sizes the caller's vector.
            std::vector<std::uint8_t> pixels;
            renderer->readPixels(pixels);

            const std::size_t before = Testing::getAllocationCount();
            renderer->readPixels(pixels);
            EXPECT_EQ(Testing::getAllocationCount() - before, 0u) << "a second read back into the same vector";
        }

        /// A texture arriving costs the device objects it is and nothing besides.
        ///
        /// **The arrival frame is the frame with the least room, which is why this is bounded at
        /// all.** A cell crossing writes a couple of hundred textures into the array on one frame,
        /// and a vector the write grows for itself is growth on top of the upload. What this leaves
        /// is what a texture *is*: two images and the staging buffer their bytes travel in, each of
        /// which is a device allocation the host has to hold a handle for.
        ///
        /// Warmed up with one texture first, so the array's own scratch and its descriptor pool have
        /// reached their size and what is measured is the arrival and not the array.
        TEST_F(RtxFrameCostTest, aTextureArrivingCostsWhatATextureIs)
        {
            constexpr std::uint32_t extent = 4;
            constexpr std::uint32_t slots = 64;
            constexpr std::uint32_t measured = 32;

            // What one texture is allowed, and what it is spent on. Two images, each a `unique_ptr`
            // the host holds for a device object; the staging buffer its bytes travel in; and the
            // names those carry, which a build that names objects makes strings for and a release
            // build does not — `Device::wantsNames`. Measured at five apiece with names on.
            //
            // The sixth is the batch's own list of what it is keeping alive and the graveyard's, both
            // of which grow as any vector does. Nothing here may grow per texture: that is what the
            // number is for, and a seventh is a buffer somebody made per arrival.
            constexpr std::size_t budgetPerTexture = 6;

            Device& device = getDevice();
            CommandPool& pool = getPool();
            Graveyard graveyard(device, pool);

            // Flat and uncompressed, so the description is exact arithmetic rather than a file.
            const std::vector<std::uint8_t> texels(std::size_t{ extent } * extent * 4, 0xFF);
            const std::array<MipLevel, 1> levels{ MipLevel{ 0, extent, extent } };
            const std::array<float, ShadingMap::sCells> shading{};

            const auto describe = [&](std::uint32_t slot) {
                return TextureData{
                    .mSlot = slot,
                    .mFormat = TextureFormat::Rgba8Unorm,
                    .mWidth = extent,
                    .mHeight = extent,
                    .mBytes = std::as_bytes(std::span(texels)),
                    .mLevels = levels,
                    .mShading = shading,
                    .mName = "arrival",
                };
            };

            Batch setup(pool);
            TextureArray array(device, setup, slots, {}, graveyard);
            setup.flush();

            const auto arrive = [&](Batch& batch, std::uint32_t slot) {
                const TextureData described = describe(slot);
                array.write(batch, std::span(&described, 1), graveyard);
            };

            {
                Batch warm(pool);
                arrive(warm, 0);
                warm.flush();
            }

            // **One batch for all of them, which is how a cell arrives**: the renderer records every
            // texture of a crossing into one and flushes once. A batch apiece would be measuring the
            // command pool rather than the array.
            Batch batch(pool);

            const std::size_t before = Testing::getAllocationCount();
            for (std::uint32_t slot = 1; slot <= measured; ++slot)
                arrive(batch, slot);
            const std::size_t after = Testing::getAllocationCount();

            batch.flush();

            EXPECT_LE(after - before, budgetPerTexture * measured)
                << (after - before) << " allocations across " << measured << " arrivals, "
                << static_cast<double>(after - before) / measured << " apiece";

            EXPECT_EQ(array.getHeld().mCount, measured + 1);
        }
    }
}

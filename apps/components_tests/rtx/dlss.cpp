#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "geometry.hpp"
#include "harness.hpp"

#ifdef OPENMW_RTX_DLSS

#include <array>
#include <cstring>
#include <vector>

#include <osg/Vec2f>

#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/gbuffer.h>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/dlss.hpp>
#include <components/rtxvulkan/dlsspass.hpp>
#include <components/rtxvulkan/image.hpp>

#include "testtexture.hpp"

namespace Rtx
{
    namespace
    {
        /// Everything DLSS reads and the one image it writes are made alike.
        ///
        /// `SAMPLED` because DLSS samples its inputs and an image it cannot sample reads as zero —
        /// no error, no validation message, a black frame. `TRANSFER_DST` so a clear can fill it and
        /// `TRANSFER_SRC` so the result can be read back.
        std::unique_ptr<Image> makeImage(
            const Device& device, VkExtent2D extent, VkFormat format, std::string_view name)
        {
            return std::make_unique<Image>(device, extent.width, extent.height, format,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                name);
        }

        /// Fills `image` with one value and leaves it in `VK_IMAGE_LAYOUT_GENERAL`, which is where
        /// the renderer's own frame leaves the G-buffer.
        void fill(CommandPool& pool, const Image& image, const std::array<float, 4>& value)
        {
            pool.submitAndWait([&](VkCommandBuffer commands) {
                image.transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);

                VkClearColorValue colour{};
                std::memcpy(colour.float32, value.data(), sizeof(colour.float32));
                const VkImageSubresourceRange whole{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdClearColorImage(
                    commands, image.getHandle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &whole);

                image.transition(commands, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT);
            });
        }

        /// NGX brought up on the shared device for the length of this suite.
        ///
        /// **One runtime, because there can only be one and it costs a quarter of a second.** NGX
        /// keeps a single runtime per process, its shutdown is unconditional, and it belongs to the
        /// device it was started on — so the tests below share this one rather than each standing up
        /// its own.
        ///
        /// **Down with the suite and not with the binary.** A renderer asked to upscale builds a
        /// runtime of its own, and a second is a throw — so `RtxUpscaledFrameTest` could not run
        /// while this one was still up.
        class RtxDlssTest : public Testing::DeviceTest
        {
        protected:
            static void SetUpTestSuite()
            {
                std::string reason;
                const Testing::Harness* harness = Testing::getHarness(reason);
                if (harness == nullptr)
                    return;

                sNgx = std::make_unique<Dlss>(*harness->mDevice, harness->mInstance->getHandle());
            }

            static void TearDownTestSuite() { sNgx.reset(); }

            void SetUp() override
            {
                Testing::DeviceTest::SetUp();
                if (mHarness == nullptr)
                    return;

                if (!sNgx->isAvailable())
                    GTEST_SKIP() << sNgx->getObstacle();
            }

            VkInstance getInstance() const { return mHarness->mInstance->getHandle(); }

            /// The extent every size question here is asked about, which is the one the frame budget
            /// is written against.
            static constexpr VkExtent2D sOutput{ 3840, 2160 };

            static inline std::unique_ptr<Dlss> sNgx;
        };

        /// **A second one is refused rather than made.** It would not stand beside the first: NGX
        /// keeps one runtime per process and its shutdown is unconditional, so the second to be
        /// destroyed would leave the first holding a feature that answers `FAIL_NotInitialized` —
        /// which nothing else here would notice.
        TEST_F(RtxDlssTest, aSecondRuntimeIsRefusedRatherThanMade)
        {
            EXPECT_THROW(Dlss(getDevice(), getInstance()), Error);
        }

        /// Asking whether Ray Reconstruction is available must not decide anything about who owns
        /// NGX, which is the whole of why `probe` is not the constructor.
        TEST_F(RtxDlssTest, theCapabilityQuestionLeavesTheRuntimeItWasAskedOf)
        {
            EXPECT_TRUE(Dlss::probe(getDevice(), getInstance()).mAvailable);

            // **The half of it the answer cannot carry.** `probe` stands a runtime up where none is
            // up and takes it down again, so one that failed to notice this one would end it — and
            // only a question asked afterwards can tell.
            EXPECT_NO_THROW(sNgx->getRenderSize(sOutput, Upscale::Performance));
        }

        /// **The frame budget's own numbers, asked of DLSS rather than assumed.** `plan.md` §5.3
        /// settles on 1920×1080 internal to 3840×2160, and Performance is the mode that ratio comes
        /// from — so if DLSS asks for something else, every figure the project is measured against
        /// was measured at the wrong resolution.
        TEST_F(RtxDlssTest, performanceRendersTheResolutionTheFrameBudgetAssumes)
        {
            const VkExtent2D render = sNgx->getRenderSize(sOutput, Upscale::Performance);
            EXPECT_EQ(render.width, 1920u);
            EXPECT_EQ(render.height, 1080u);
        }

        /// The modes have to differ, and in the direction their names claim: a query that ignored
        /// the quality value would answer the same size for all four and look plausible.
        TEST_F(RtxDlssTest, eachModeRendersMoreThanTheModeBelowIt)
        {
            const VkExtent2D performance = sNgx->getRenderSize(sOutput, Upscale::Performance);
            const VkExtent2D balanced = sNgx->getRenderSize(sOutput, Upscale::Balanced);
            const VkExtent2D quality = sNgx->getRenderSize(sOutput, Upscale::Quality);
            const VkExtent2D dlaa = sNgx->getRenderSize(sOutput, Upscale::Dlaa);

            EXPECT_LT(performance.width, balanced.width);
            EXPECT_LT(balanced.width, quality.width);
            EXPECT_LT(quality.width, dlaa.width);

            // DLAA is one to one by definition, which is what makes it the control for "how much of
            // the softness is the upscale".
            EXPECT_EQ(dlaa.width, sOutput.width);
            EXPECT_EQ(dlaa.height, sOutput.height);
        }

        /// **The mode that is not one is refused rather than answered.** `Off` used to share a
        /// `switch` arm with `Performance` so the switch was total, which made "build a feature for
        /// the setting that means build no feature" answer with the fastest and softest mode this
        /// renderer has — silently, on the path a frame budget is measured against.
        TEST_F(RtxDlssTest, theAbsenceOfAnUpscalerNamesNoSizeToRenderAt)
        {
            EXPECT_THROW(sNgx->getRenderSize(sOutput, Upscale::Off), Error);
        }

        /// **A flat frame is the one input whose correct output is arithmetic** rather than a
        /// reimplementation of the network: upscaling a constant field can only produce that field.
        ///
        /// The build is what this shares with nothing else here — it uploads the network's weights,
        /// so it is the first call that does real work on the device rather than answering from a
        /// table, and the first place a wrong parameter map shows up as anything but a query result.
        TEST_F(RtxDlssTest, aFlatFrameResolvesToItself)
        {
            const Device& device = getDevice();
            CommandPool& pool = getPool();
            const VkExtent2D render = sNgx->getRenderSize(sOutput, Upscale::Performance);

            // **Built for a named preset, which is the first thing a wrong parameter map would
            // refuse.** A hint set under the wrong name is not an error to NGX — it reverts to
            // whatever the installed library defaults to and says nothing — so what this proves is
            // only that the build accepts one. That the network actually changes with it is a
            // picture question and is measured with `shot --preset`.
            std::unique_ptr<DlssPass> pass;
            pool.submitAndWait([&](VkCommandBuffer commands) {
                pass = std::make_unique<DlssPass>(*sNgx, commands, render, sOutput, Upscale::Performance, Preset::D);
            });

            // **Every input in the format `GBuffer` gives it, and named rather than spelled.** What
            // this test proves is that NGX takes the parameter map the renderer builds, and it
            // proves nothing about a map built out of images the renderer never hands over — the
            // masks were four bytes here and one byte there, and the two albedos and the guide were
            // full floats here and halves there. Naming them is also what makes a format changed in
            // `gbuffer.h` reach this test rather than drift away from it.
            //
            // The colour and the output are not the g-buffer's: `VulkanRenderer` makes both at full
            // float directly, and these follow that.
            const std::unique_ptr<Image> colour
                = makeImage(device, render, VK_FORMAT_R32G32B32A32_SFLOAT, "test-colour");
            const std::unique_ptr<Image> diffuse = makeImage(device, render, GBUFFER_ALBEDO, "test-diffuse");
            const std::unique_ptr<Image> specular = makeImage(device, render, GBUFFER_ALBEDO, "test-specular");
            const std::unique_ptr<Image> normals = makeImage(device, render, GBUFFER_GUIDE, "test-normals");
            const std::unique_ptr<Image> depth = makeImage(device, render, GBUFFER_DEPTH, "test-depth");
            const std::unique_ptr<Image> motion = makeImage(device, render, GBUFFER_MOTION, "test-motion");
            const std::unique_ptr<Image> reflections = makeImage(device, render, GBUFFER_MOTION, "test-reflections");
            const std::unique_ptr<Image> particles = makeImage(device, render, GBUFFER_MASK, "test-particles");
            const std::unique_ptr<Image> bias = makeImage(device, render, GBUFFER_MASK, "test-bias");
            const std::unique_ptr<Image> layer = makeImage(device, render, GBUFFER_LAYER, "test-transparency");
            const std::unique_ptr<Image> layerOpacity
                = makeImage(device, render, GBUFFER_LAYER_OPACITY, "test-transparency-opacity");
            const std::unique_ptr<Image> layerMotion
                = makeImage(device, render, GBUFFER_MOTION, "test-transparency-motion");
            const std::unique_ptr<Image> output
                = makeImage(device, sOutput, VK_FORMAT_R32G32B32A32_SFLOAT, "test-output");

            // A frame with nothing in it to resolve: uniform radiance over a flat wall halfway down
            // the depth range, facing the camera, stationary and fully rough.
            fill(pool, *colour, { 0.25f, 0.5f, 0.75f, 1.0f });
            fill(pool, *diffuse, { 0.5f, 0.5f, 0.5f, 1.0f });
            fill(pool, *specular, { 0.04f, 0.04f, 0.04f, 1.0f });
            fill(pool, *normals, { 0.0f, 0.0f, 1.0f, 1.0f });
            fill(pool, *depth, { 0.5f, 0.0f, 0.0f, 0.0f });
            fill(pool, *motion, { 0.0f, 0.0f, 0.0f, 0.0f });
            // No sprite reached this frame and nothing about it is untrustworthy, which is the
            // state that has to leave the picture alone.
            fill(pool, *reflections, { 0.0f, 0.0f, 0.0f, 0.0f });
            fill(pool, *particles, { 0.0f, 0.0f, 0.0f, 0.0f });
            fill(pool, *bias, { 0.0f, 0.0f, 0.0f, 0.0f });
            fill(pool, *output, { 0.0f, 0.0f, 0.0f, 0.0f });

            mHarness->mInstance->getValidationLog()->clear();

            pool.submitAndWait([&](VkCommandBuffer commands) {
                pass->record(commands,
                    DlssInputs{
                        .mColour = *colour,
                        .mDiffuseAlbedo = *diffuse,
                        .mSpecularAlbedo = *specular,
                        .mNormalRoughness = *normals,
                        .mDepth = *depth,
                        .mMotion = *motion,
                        .mReflectionMotion = *reflections,
                        .mParticleMask = *particles,
                        .mTransparency = *layer,
                        .mTransparencyOpacity = *layerOpacity,
                        .mTransparencyMotion = *layerMotion,
                        .mBiasMask = *bias,
                        .mOutput = *output,
                        .mJitter = osg::Vec2f(0.0f, 0.0f),
                        // The first frame has no history, which is what a reset means.
                        .mReset = true,
                    });
            });

            std::vector<std::uint8_t> bytes;
            output->read(pool, VK_IMAGE_LAYOUT_GENERAL, bytes);
            ASSERT_EQ(bytes.size(), std::size_t{ sOutput.width } * sOutput.height * 16);

            std::vector<float> pixels(bytes.size() / sizeof(float));
            std::memcpy(pixels.data(), bytes.data(), bytes.size());

            // Away from the border, where the network has no neighbourhood and rolls off.
            const std::size_t centre = (std::size_t{ sOutput.height / 2 } * sOutput.width + sOutput.width / 2) * 4;

            // Three different values rather than one grey, because a single channel read twice would
            // pass a grey check while proving nothing about which channel was read.
            constexpr std::array<float, 3> sExpected{ 0.25f, 0.5f, 0.75f };
            for (std::size_t channel = 0; channel < sExpected.size(); ++channel)
                EXPECT_NEAR(pixels[centre + channel], sExpected[channel], sExpected[channel] * 0.05f)
                    << "channel " << channel << " of a flat frame did not resolve to itself";

            // **The floor a rejected input reads back as, and the reason this assertion is here
            // beside the one above.** An image DLSS cannot sample is not an error anywhere: NGX
            // returns success, the validation layers say nothing, and the network resolves the black
            // field it saw to a uniform value near zero — 1.36e-7 here, measured by dropping
            // `VK_IMAGE_USAGE_SAMPLED_BIT` from the images above.
            EXPECT_GT(pixels[centre], 1e-6f) << "the output is at the epsilon floor, so DLSS resolved "
                                                "an input it never read";

            // **DLSS records its own commands into that buffer**, and success says only that NGX
            // liked the parameter map — not that what it recorded was valid. The layers are what
            // have an opinion about the resources it then touched.
            for (const ValidationMessage& message : mHarness->mInstance->getValidationLog()->getErrorsOnThisThread())
                ADD_FAILURE() << "validation error from the evaluation: " << message.mText;
        }

        /// The mean of one channel over a frame `readPixels` gave back.
        double channelMeanOf(const std::vector<std::uint8_t>& pixels, std::size_t channel)
        {
            double total = 0.0;
            for (std::size_t at = channel; at < pixels.size(); at += 4)
                total += pixels[at];

            return total / (static_cast<double>(pixels.size()) / 4.0);
        }

        /// **A fixture of its own because it must not inherit the one above.** A renderer asked to
        /// upscale brings up an NGX runtime of its own, and `RtxDlssTest` holds one for the length
        /// of its suite.
        struct RtxUpscaledFrameTest : Testing::RendererTest
        {
            /// A renderer of the test's own that upscales to `width` by `height`, or null with the
            /// reason in `reason`. Beside `mRenderer` and not instead of it, for the two seconds
            /// `Testing::getRenderer` says a second one costs.
            static std::unique_ptr<Renderer> makeUpscaling(
                std::uint32_t width, std::uint32_t height, std::string& reason)
            {
                RendererOptions options = Testing::describeRenderer(width, height);
                options.mUpscale = Upscale::Performance;
                return createRenderer(options, reason);
            }
        };

        /// The whole frame through the renderer, against the same frame with nothing upscaling it.
        ///
        /// **What the pass's own test cannot reach.** That one hands NGX images it filled itself;
        /// this one asks whether the renderer wired them up — the extents, the layouts, the barrier
        /// after the composite, the jitter, and which image the curve ends up reading. Every one of
        /// those failures produces a frame, and most of them produce a black one.
        ///
        /// **Upscaling preserves the average**, which is the claim being made: four times the pixels
        /// reconstructed from the same light is the same picture larger, not a brighter or darker
        /// one. It is a weak claim about sharpness and a strong one about everything that goes wrong
        /// here, since a frame that lost an input, read the wrong image, or skipped the curve is not
        /// off by five per cent but by all of it.
        ///
        /// **At an output the ratio does not divide, deliberately.** 1281 by 721 renders at 641 by
        /// 361, which is a hair under half rather than exactly half — and a guide the upscaler reads
        /// only there is one nothing at a round resolution can see. The transparency layer's alpha
        /// was such a guide: handed over as one it says the layer covers every pixel, which resolved
        /// the whole frame to a layer that was black wherever no sprite reached. Every extent this
        /// suite measured at was an exact fraction of its output, so nothing here failed while every
        /// window whose width the ratio does not divide drew a black frame.
        TEST_F(RtxUpscaledFrameTest, anUpscaledFrameIsTheSameFrameLarger)
        {
            std::string reason;
            const std::unique_ptr<Renderer> upscaling = makeUpscaling(1281, 721, reason);
            if (upscaling == nullptr)
                GTEST_SKIP() << reason;

            const FrameExtents extents = upscaling->getExtents();
            EXPECT_EQ(extents.mOutputWidth, 1281u);
            EXPECT_EQ(extents.mOutputHeight, 721u);
            EXPECT_NE(extents.mOutputWidth, extents.mRenderWidth * 2u) << "an exact half hides what this is for";
            EXPECT_LT(extents.mRenderWidth, extents.mOutputWidth);
            EXPECT_LT(extents.mRenderHeight, extents.mOutputHeight);

            // A wall four hundred units across, larger than the frame, lit by one sun and no sky —
            // so every pixel is the same surface and nothing in the picture is background.
            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(Testing::sWallQuad, {}, {}, Testing::sQuadIndices) });

            // **One camera for both, and it is built for the render extent**, because that is what
            // both renderers trace at — the upscaler only changes what happens after.
            Shaders::VisibilityConstants camera = makeCamera(osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(), 60.0f,
                extents.mRenderWidth, extents.mRenderHeight, 10000.0f);
            camera.mSunPosition = osg::Vec3f(0.0f, -0.6f, -0.8f);
            camera.mSunIrradiance = osg::Vec3f(2.0f, 2.0f, 2.0f);
            camera.mSkyHorizon = osg::Vec3f();
            camera.mSkyZenith = osg::Vec3f();

            std::vector<std::uint8_t> reference;
            mRenderer->resize(extents.mRenderWidth, extents.mRenderHeight);
            mRenderer->setScene(Rtx::sWorld, scene, {}, SeaState{});
            mRenderer->renderFrame(camera, FrameOptions{ .mFilter = false });
            mRenderer->readPixels(reference);

            // **Several frames, because a temporal upscaler has nothing on the first.** The camera
            // does not move, so what the run buys is history rather than a different picture.
            constexpr std::uint32_t sFrames = 8;
            upscaling->setScene(Rtx::sWorld, scene, {}, SeaState{});
            for (std::uint32_t frame = 0; frame < sFrames; ++frame)
            {
                camera.mFrame = frame;
                upscaling->renderFrame(camera, FrameOptions{});
            }

            std::vector<std::uint8_t> upscaled;
            upscaling->readPixels(upscaled);

            ASSERT_EQ(reference.size(), std::size_t{ extents.mRenderWidth } * extents.mRenderHeight * 4);
            ASSERT_EQ(upscaled.size(), std::size_t{ extents.mOutputWidth } * extents.mOutputHeight * 4);

            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const double was = channelMeanOf(reference, channel);
                const double now = channelMeanOf(upscaled, channel);
                EXPECT_GT(was, 1.0) << "channel " << channel << " of the reference is black, so it proves nothing";
                EXPECT_NEAR(now, was, was * 0.05)
                    << "channel " << channel << " came out of the upscaler at a different exposure";
            }

            std::vector<std::string> errors;
            upscaling->takeValidationErrors(errors);
            for (const std::string& error : errors)
                ADD_FAILURE() << "validation error from the upscaled frame: " << error;
        }

        /// A sprite carries its own travel into the layer whatever share of a pixel it took.
        ///
        /// **The rain, which never owns a pixel and is the whole layer of the ones it reaches.**
        /// `puffClaim` decides which of the sprite and the surface owns the *frame's* one vector,
        /// and a fifth of a pixel of sprite rightly loses that: the pixel is mostly the wall. The
        /// layer is not the pixel — it holds the sprites and nothing else — so the same test applied
        /// there handed the drops the wall's vector, and Ray Reconstruction, told that layer may be
        /// accumulated, held the storm against whatever the player was walking past.
        ///
        /// A fifth of a pixel of sprite, a hundred units ahead of the eye, travelling ten units
        /// across: `height * 10 / (2 * 100 * tan 30°)` pixels of screen motion, and a wall two
        /// hundred units out that did not move at all.
        ///
        /// An upscaling renderer because nothing else writes the layer.
        TEST_F(RtxUpscaledFrameTest, aSpriteCarriesItsOwnMotionIntoTheLayerItIsTheWholeOf)
        {
            std::string reason;
            const std::unique_ptr<Renderer> upscaling = makeUpscaling(721, 721, reason);
            if (upscaling == nullptr)
                GTEST_SKIP() << reason;

            const FrameExtents extents = upscaling->getExtents();
            const std::uint32_t width = extents.mRenderWidth;
            const std::uint32_t height = extents.mRenderHeight;
            const std::size_t centre = (std::size_t{ height / 2 } * width + width / 2) * 2;

            constexpr float travel = 10.0f;
            constexpr float ahead = 100.0f;
            const float expected = static_cast<float>(height) * travel / (2.0f * ahead * 0.5773503f);

            constexpr std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const std::array<TextureData, 1> puff{ Testing::describeTexel(white) };

            Shaders::VisibilityConstants camera
                = makeCamera(osg::Vec3f(0.0f, -200.0f, 0.0f), osg::Vec3f(), 60.0f, width, height, 10000.0f);
            camera.mSunPosition = osg::Vec3f(0.0f, -0.6f, -0.8f);
            camera.mSunIrradiance = osg::Vec3f(2.0f, 2.0f, 2.0f);

            // **The layer's vector for a sprite that travelled `moved`, at the middle of the
            // frame.** Two frames, because the first has no past to reproject against.
            const auto layerMotionAtCentre = [&](const osg::Vec3f& moved, std::vector<float>& frameMotion) {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(Testing::sWallQuad, {}, {}, Testing::sQuadIndices) });

                const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));

                // A fifth, which is under the half `puffClaim` asks for and over nothing at all.
                const std::array<Sprite, 1> sprites{ Sprite{
                    .mPosition = osg::Vec3f(0.0f, -ahead, 0.0f), .mRadius = 20.0f, .mAlpha = 0.2f, .mMoved = moved } };
                scene.addEmitter(sprites, cut, false);

                upscaling->setScene(Rtx::sWorld, scene, puff, SeaState{});
                upscaling->renderFrame(camera, FrameOptions{ .mFilter = false });
                upscaling->renderFrame(camera, FrameOptions{ .mFilter = false });

                std::vector<float> layer;
                upscaling->readChannel(Channel::TransparencyMotion, layer);
                upscaling->readChannel(Channel::Motion, frameMotion);
                return layer;
            };

            std::vector<float> frameMotion;
            const std::vector<float> travelled = layerMotionAtCentre(osg::Vec3f(travel, 0.0f, 0.0f), frameMotion);
            ASSERT_EQ(travelled.size(), std::size_t{ width } * height * 2);

            std::vector<float> particles;
            upscaling->readChannel(Channel::ParticleMask, particles);
            ASSERT_EQ(particles[centre / 2], 1.0f) << "the sprite reached the middle of the frame";

            EXPECT_NEAR(std::abs(travelled[centre]), expected, 1.0f)
                << "the sprite's own travel, projected at the depth it hangs at";
            EXPECT_NEAR(travelled[centre + 1], 0.0f, 1.0f) << "and it travelled across rather than along";

            EXPECT_NEAR(frameMotion[centre], 0.0f, 0.01f) << "the pixel is mostly the wall, and the wall stood still";

            // **The parameter has to matter**, or this measures a coincidence: the same frame with a
            // sprite that did not move writes the wall's nought into the layer as well.
            const std::vector<float> stood = layerMotionAtCentre(osg::Vec3f(), frameMotion);
            EXPECT_NEAR(stood[centre], 0.0f, 0.01f) << "a sprite that stood still moved nothing";
        }
    }
}

#else

namespace
{
    /// **Skipped rather than absent**, so a build with no DLSS says so instead of quietly running
    /// fewer tests.
    ///
    /// One test for the file rather than a stub per test above, because a stub written per name is
    /// a list that stops matching the moment a test is added on the other side of the `#ifdef`. Its
    /// own suite name for the same reason it is a bare `TEST`: the suite above is `TEST_F`, and one
    /// suite cannot hold both forms.
    TEST(RtxDlss, thisBuildHasNoRayReconstruction)
    {
        GTEST_SKIP() << "this build has no DLSS; configure with -DOPENMW_RTX_DLSS=ON";
    }
}

#endif

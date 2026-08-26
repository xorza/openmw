#include <memory>

#include <string>

#include <gtest/gtest.h>

#include "harness.hpp"

#ifdef OPENMW_RTX_DLSS

#include <array>
#include <cstring>
#include <vector>

#include <osg/Vec2f>

#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/dlss.hpp>
#include <components/rtxvulkan/dlsspass.hpp>
#include <components/rtxvulkan/image.hpp>

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

        /// NGX brought up, asked what it wants, built, and run once over a frame whose answer is
        /// known.
        ///
        /// **One test rather than five, because NGX is global to the process and keyed by device.**
        /// Two of these alive at once is not something the SDK promises to survive, and the five
        /// questions share the one expensive setup.
        TEST(RtxDlssTest, rayReconstructionBuildsAndResolvesAFlatFrame)
        {
            std::string reason;
            Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            const Device& device = *harness->mDevice;
            const Dlss ngx(device, harness->mInstance->getHandle());
            if (!ngx.isAvailable())
                GTEST_SKIP() << ngx.getObstacle();

            // **A second one is refused rather than made.** It would not stand beside this one: NGX
            // keeps one runtime per process and its shutdown is unconditional, so the second to be
            // destroyed would leave the first holding a feature that answers `FAIL_NotInitialized`
            // — which nothing else here would notice.
            EXPECT_THROW(Dlss(device, harness->mInstance->getHandle()), Error);

            // And asking the capability question does not disturb it, which is what `probe` is for.
            EXPECT_TRUE(Dlss::probe(device, harness->mInstance->getHandle()).mAvailable);

            // **The frame budget's own numbers, asked of DLSS rather than assumed.** `plan.md` §5.3
            // settles on 1920×1080 internal to 3840×2160, and Performance is the mode that ratio
            // comes from — so if DLSS asks for something else, every figure the project is measured
            // against was measured at the wrong resolution.
            constexpr VkExtent2D sOutput{ 3840, 2160 };
            const VkExtent2D render = ngx.getRenderSize(sOutput, Upscale::Performance);
            EXPECT_EQ(render.width, 1920u);
            EXPECT_EQ(render.height, 1080u);

            // The other modes have to differ, and in the direction their names claim: a query that
            // ignored the quality value would answer the same size for all four and look plausible.
            const VkExtent2D balanced = ngx.getRenderSize(sOutput, Upscale::Balanced);
            const VkExtent2D quality = ngx.getRenderSize(sOutput, Upscale::Quality);
            const VkExtent2D dlaa = ngx.getRenderSize(sOutput, Upscale::Dlaa);
            EXPECT_LT(render.width, balanced.width);
            EXPECT_LT(balanced.width, quality.width);
            EXPECT_LT(quality.width, dlaa.width);
            // DLAA is one to one by definition, which is what makes it the control for "how much of
            // the softness is the upscale".
            EXPECT_EQ(dlaa.width, sOutput.width);
            EXPECT_EQ(dlaa.height, sOutput.height);

            // **And the mode that is not one is refused rather than answered.** `Off` used to share
            // a `switch` arm with `Performance` so the switch was total, which made "build a feature
            // for the setting that means build no feature" answer with the fastest and softest mode
            // this renderer has — silently, on the path a frame budget is measured against.
            EXPECT_THROW(ngx.getRenderSize(sOutput, Upscale::Off), Error)
                << "the absence of an upscaler names no size to render at";

            CommandPool pool(device);

            // Building uploads the network's weights, so this is the first call that does real work
            // on the device rather than answering from a table — and the first place a wrong
            // parameter map shows up as anything other than a query result.
            // **Built for a named preset, which is the first thing a wrong parameter map would refuse.**
            // A hint set under the wrong name is not an error to NGX — it reverts to whatever the
            // installed library defaults to and says nothing — so what this proves is only that the
            // build accepts one. That the network actually changes with it is a picture question and
            // is measured with `shot --preset`.
            std::unique_ptr<DlssPass> pass;
            pool.submitAndWait([&](VkCommandBuffer commands) {
                pass = std::make_unique<DlssPass>(ngx, commands, render, sOutput, Upscale::Performance, Preset::D);
            });

            const std::unique_ptr<Image> colour
                = makeImage(device, render, VK_FORMAT_R32G32B32A32_SFLOAT, "test-colour");
            const std::unique_ptr<Image> diffuse
                = makeImage(device, render, VK_FORMAT_R32G32B32A32_SFLOAT, "test-diffuse");
            const std::unique_ptr<Image> specular
                = makeImage(device, render, VK_FORMAT_R32G32B32A32_SFLOAT, "test-specular");
            const std::unique_ptr<Image> normals
                = makeImage(device, render, VK_FORMAT_R32G32B32A32_SFLOAT, "test-normals");
            const std::unique_ptr<Image> depth = makeImage(device, render, VK_FORMAT_R32_SFLOAT, "test-depth");
            const std::unique_ptr<Image> motion = makeImage(device, render, VK_FORMAT_R32G32_SFLOAT, "test-motion");
            // The two the frame writes for what it composites in front of its surfaces. One float
            // apiece for the reason `gbuffer.h` gives.
            const std::unique_ptr<Image> reflections
                = makeImage(device, render, VK_FORMAT_R32G32_SFLOAT, "test-reflections");
            const std::unique_ptr<Image> particles = makeImage(device, render, VK_FORMAT_R32_SFLOAT, "test-particles");
            const std::unique_ptr<Image> bias = makeImage(device, render, VK_FORMAT_R32_SFLOAT, "test-bias");
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

            harness->mInstance->getValidationLog()->clear();

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

            // **A flat frame is the one input whose correct output is arithmetic** rather than a
            // reimplementation of the network: upscaling a constant field can only produce that
            // field. Three different values rather than one grey, because a single channel read
            // twice would pass a grey check while proving nothing about which channel was read.
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
            for (const ValidationMessage& message : harness->mInstance->getValidationLog()->getErrorsOnThisThread())
                ADD_FAILURE() << "validation error from the evaluation: " << message.mText;

            // Released here rather than at the end of the scope, so a failure to release is this
            // test's and not the next one's: NGX keeps its state per device.
            pass.reset();
        }

        /// The mean of one channel over a frame `readPixels` gave back.
        double meanOf(const std::vector<std::uint8_t>& pixels, std::size_t channel)
        {
            double total = 0.0;
            for (std::size_t at = channel; at < pixels.size(); at += 4)
                total += pixels[at];

            return total / (static_cast<double>(pixels.size()) / 4.0);
        }

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
        TEST(RtxDlssTest, anUpscaledFrameIsTheSameFrameLarger)
        {
            std::string reason;
            Renderer* plain = Testing::getRenderer(reason);
            if (plain == nullptr)
                GTEST_SKIP() << reason;

            RendererOptions options;
            options.mShaderDirectory = Testing::getShaderDirectory();
            options.mWidth = 1280;
            options.mHeight = 720;
            options.mUpscale = Upscale::Performance;
            options.mValidation.mEnabled = true;
            options.mValidation.mAbortOnError = false;

            const std::unique_ptr<Renderer> upscaling = createRenderer(options, reason);
            if (upscaling == nullptr)
                GTEST_SKIP() << reason;

            const FrameExtents extents = upscaling->getExtents();
            EXPECT_EQ(extents.mOutputWidth, 1280u);
            EXPECT_EQ(extents.mOutputHeight, 720u);
            EXPECT_LT(extents.mRenderWidth, extents.mOutputWidth);
            EXPECT_LT(extents.mRenderHeight, extents.mOutputHeight);

            // A wall four hundred units across, larger than the frame, lit by one sun and no sky —
            // so every pixel is the same surface and nothing in the picture is background.
            SceneDesc scene;
            const std::array<osg::Vec3f, 4> quad{
                osg::Vec3f(-200.0f, 0.0f, -200.0f),
                osg::Vec3f(200.0f, 0.0f, -200.0f),
                osg::Vec3f(200.0f, 0.0f, 200.0f),
                osg::Vec3f(-200.0f, 0.0f, 200.0f),
            };
            constexpr std::array<std::uint32_t, 6> indices{ 0, 1, 2, 0, 2, 3 };
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(quad, {}, {}, indices) });

            // **One camera for both, and it is built for the render extent**, because that is what
            // both renderers trace at — the upscaler only changes what happens after.
            Shaders::VisibilityConstants camera = makeCamera(osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(), 60.0f,
                extents.mRenderWidth, extents.mRenderHeight, 10000.0f);
            camera.mSunPosition = osg::Vec3f(0.0f, -0.6f, -0.8f);
            camera.mSunIrradiance = osg::Vec3f(2.0f, 2.0f, 2.0f);
            camera.mSkyHorizon = osg::Vec3f();
            camera.mSkyZenith = osg::Vec3f();

            std::vector<std::uint8_t> reference;
            plain->resize(extents.mRenderWidth, extents.mRenderHeight);
            plain->setScene(Rtx::sWorld, scene, {}, SeaState{});
            plain->renderFrame(camera, FrameOptions{ .mFilter = false });
            plain->readPixels(reference);

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
                const double was = meanOf(reference, channel);
                const double now = meanOf(upscaled, channel);
                EXPECT_GT(was, 1.0) << "channel " << channel << " of the reference is black, so it proves nothing";
                EXPECT_NEAR(now, was, was * 0.05)
                    << "channel " << channel << " came out of the upscaler at a different exposure";
            }

            std::vector<std::string> errors;
            upscaling->takeValidationErrors(errors);
            for (const std::string& error : errors)
                ADD_FAILURE() << "validation error from the upscaled frame: " << error;
        }

    }
}

#else

namespace
{
    /// **Skipped rather than absent**, so a build with no DLSS says so once per test that needs it
    /// instead of quietly running fewer.
    TEST(RtxDlssTest, rayReconstructionBuildsAndResolvesAFlatFrame)
    {
        GTEST_SKIP() << "this build has no DLSS; configure with -DOPENMW_RTX_DLSS=ON";
    }

    TEST(RtxDlssTest, anUpscaledFrameIsTheSameFrameLarger)
    {
        GTEST_SKIP() << "this build has no DLSS; configure with -DOPENMW_RTX_DLSS=ON";
    }

}

#endif

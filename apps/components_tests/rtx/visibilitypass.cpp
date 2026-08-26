#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/instancerecord.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/accumulate.h>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/compositepass.hpp>
#include <components/rtxvulkan/gbuffer.hpp>
#include <components/rtxvulkan/image.hpp>
#include <components/rtxvulkan/result.hpp>
#include <components/rtxvulkan/sceneacceleration.hpp>
#include <components/rtxvulkan/scenebuffers.hpp>
#include <components/rtxvulkan/texture.hpp>
#include <components/rtxvulkan/visibilitypass.hpp>

#include "allocations.hpp"
#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        /// OpenSceneGraph's transform and an instance descriptor's must move a point to the same
        /// place.
        ///
        /// OSG multiplies a row vector on the left and a descriptor a column vector on the right, so
        /// the conversion is a transpose with the translation moved from the last row to the last
        /// column. Getting it wrong mirrors the world about its diagonal, which symmetrical
        /// architecture hides well enough to survive being looked at.
        ///
        /// Asserted on `Transform3x4`, which is where that transposition happens for every backend.
        TEST(RtxTransformTest, theNeutralTransformMovesAPointWhereOpenSceneGraphWould)
        {
            osg::Matrixf matrix = osg::Matrixf::scale(2.0f, 2.0f, 2.0f)
                * osg::Matrixf::rotate(osg::DegreesToRadians(37.0f), osg::Vec3f(0.3f, -0.5f, 0.8f))
                * osg::Matrixf::translate(11.0f, -23.0f, 5.0f);

            const osg::Vec3f point(3.0f, -5.0f, 7.0f);
            const osg::Vec3f expected = point * matrix;

            const Transform3x4 transform = toTransform3x4(matrix);
            for (int row = 0; row < 3; ++row)
            {
                const float actual = transform.mRows[row][0] * point.x() + transform.mRows[row][1] * point.y()
                    + transform.mRows[row][2] * point.z() + transform.mRows[row][3];
                EXPECT_NEAR(actual, expected[row], 1e-3f) << "row " << row;
            }
        }

        /// Vulkan stores the same three rows of four, so its conversion must not reorder anything.
        ///
        /// Cheap, and it is the assertion a second backend copies: whatever `MTLPackedFloat4x3` or
        /// anything else stores, it has to come back to these twelve numbers in this order.
        TEST(RtxTransformTest, theVulkanTransformRestatesTheNeutralRowsUnchanged)
        {
            const Transform3x4 transform{ { { 1.0f, 2.0f, 3.0f, 4.0f }, { 5.0f, 6.0f, 7.0f, 8.0f },
                { 9.0f, 10.0f, 11.0f, 12.0f } } };

            const VkTransformMatrixKHR converted = toVulkanTransform(transform);
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 4; ++column)
                    EXPECT_EQ(converted.matrix[row][column], transform.mRows[row][column])
                        << "row " << row << " column " << column;
        }

        TEST(RtxCameraTest, theBasisIsRightHandedAboutTheWorldsUpAxis)
        {
            // Looking along +Y from the origin, 90 degrees of vertical field of view, square image:
            // the half-extents at unit distance are both tan(45) = 1.
            const Shaders::VisibilityConstants camera
                = makeCamera(osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1.0f, 0.0f), 90.0f, 100, 100, 1000.0f);

            EXPECT_NEAR(camera.mCamera.mForward.y(), 1.0f, 1e-5f);
            EXPECT_NEAR(camera.mCamera.mRight.x(), 1.0f, 1e-5f);
            EXPECT_NEAR(camera.mCamera.mUp.z(), 1.0f, 1e-5f);
        }

        TEST(RtxCameraTest, aWiderImageWidensTheHorizontalExtentAndLeavesTheVerticalAlone)
        {
            const Shaders::VisibilityConstants wide
                = makeCamera(osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1.0f, 0.0f), 90.0f, 200, 100, 1000.0f);

            EXPECT_NEAR(wide.mCamera.mRight.x(), 2.0f, 1e-5f);
            EXPECT_NEAR(wide.mCamera.mUp.z(), 1.0f, 1e-5f);
        }

        /// These come off a command line, so they are input and get a message rather than an assert
        /// that a release build would drop on the floor — leaving a normalised zero vector to fill
        /// the image with NaN and report nothing.
        TEST(RtxCameraTest, aCameraWithNoBasisIsRejectedRatherThanProducingNaN)
        {
            EXPECT_THROW(
                makeCamera(osg::Vec3f(1.0f, 2.0f, 3.0f), osg::Vec3f(1.0f, 2.0f, 3.0f), 60.0f, 64, 64, 1.0f), Error);

            EXPECT_THROW(
                makeCamera(osg::Vec3f(0.0f, 0.0f, 100.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, 64, 64, 1.0f), Error);
        }

        /// `makeCameraFromView` reads the basis out of the matrix; `makeCamera` rebuilds it from
        /// the world's up. Where both can express the viewpoint they have to agree exactly, because
        /// one of them is about to be used for viewpoints the other refuses.
        TEST(RtxCameraTest, aViewMatrixNamesTheSameCameraTheTwoWorldPointsDid)
        {
            const osg::Vec3f eye(120.0f, -45.0f, 30.0f);
            const osg::Vec3f at(-10.0f, 70.0f, 12.0f);

            const Shaders::VisibilityConstants aimed = makeCamera(eye, at, 47.0f, 320, 200, 5000.0f);
            const Shaders::VisibilityConstants viewed = makeCameraFromView(
                osg::Matrixf::lookAt(eye, at, osg::Vec3f(0.0f, 0.0f, 1.0f)), 47.0f, 320, 200, 1.0f, 5000.0f);

            for (int axis = 0; axis < 3; ++axis)
            {
                EXPECT_NEAR(viewed.mOrigin[axis], aimed.mOrigin[axis], 1e-3f) << "origin " << axis;
                EXPECT_NEAR(viewed.mCamera.mForward[axis], aimed.mCamera.mForward[axis], 1e-5f) << "forward " << axis;
                EXPECT_NEAR(viewed.mCamera.mRight[axis], aimed.mCamera.mRight[axis], 1e-5f) << "right " << axis;
                EXPECT_NEAR(viewed.mCamera.mUp[axis], aimed.mCamera.mUp[axis], 1e-5f) << "up " << axis;
            }

            EXPECT_EQ(viewed.mCamera.mOrthographic, 0u);
            EXPECT_NEAR(viewed.mCamera.mSpreadAngle, aimed.mCamera.mSpreadAngle, 1e-7f);
        }

        /// Straight down, which is the viewpoint `makeCamera` has no roll for and refuses — and it
        /// is the only viewpoint a map ever has.
        ///
        /// The extents are the box in world units and not an angle: half of two hundred across and
        /// half of a hundred down, on the axes `lookAt` puts them.
        TEST(RtxCameraTest, anOrthographicCameraCarriesItsBoxRatherThanAFieldOfView)
        {
            const osg::Matrixf view
                = osg::Matrixf::lookAt(osg::Vec3f(0.0f, 0.0f, 100.0f), osg::Vec3f(), osg::Vec3f(0.0f, 1.0f, 0.0f));

            const Shaders::VisibilityConstants camera
                = makeOrthographicCameraFromView(view, 200.0f, 100.0f, 64, 32, 5.0f, 400.0f);

            EXPECT_EQ(camera.mCamera.mOrthographic, 1u);

            EXPECT_NEAR(camera.mOrigin.z(), 100.0f, 1e-4f);
            EXPECT_NEAR(camera.mCamera.mForward.z(), -1.0f, 1e-5f);
            EXPECT_NEAR(camera.mCamera.mRight.x(), 100.0f, 1e-4f);
            EXPECT_NEAR(camera.mCamera.mUp.y(), 50.0f, 1e-4f);

            // No angle, because a parallel ray's cone does not widen; the shader takes the pixel's
            // constant footprint off `mRight` instead.
            EXPECT_EQ(camera.mCamera.mSpreadAngle, 0.0f);

            // What `makeCamera` says to the same viewpoint, and why this function exists.
            EXPECT_THROW(makeCamera(osg::Vec3f(0.0f, 0.0f, 100.0f), osg::Vec3f(), 60.0f, 64, 32, 400.0f), Error);

            EXPECT_THROW(makeOrthographicCameraFromView(view, 0.0f, 100.0f, 64, 32, 5.0f, 400.0f), Error);
        }

        /// Two triangles of a quad, wound so its face points the way its corners were listed.
        constexpr std::array<std::uint32_t, 6> sQuadIndices{ 0, 1, 2, 0, 2, 3 };

        /// The unit square, in the same corner order — a texture laid once across a quad.
        const std::array<osg::Vec2f, 4> sQuadUv{
            osg::Vec2f(0.0f, 0.0f),
            osg::Vec2f(1.0f, 0.0f),
            osg::Vec2f(1.0f, 1.0f),
            osg::Vec2f(0.0f, 1.0f),
        };

        /// A level square of `extent` about the origin at height `z`, facing up.
        std::array<osg::Vec3f, 4> makeSheet(float extent, float z)
        {
            return {
                osg::Vec3f(-extent, -extent, z),
                osg::Vec3f(extent, -extent, z),
                osg::Vec3f(extent, extent, z),
                osg::Vec3f(-extent, extent, z),
            };
        }

        /// A level sheet of water `extent` across at z = 0, with nothing under it.
        SceneDesc makeOpenWater(float extent)
        {

            SceneDesc scene;
            Material water;
            water.mKind = MaterialKind::Water;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(extent, 0.0f), {}, {}, sQuadIndices),
                .mMaterial = scene.addMaterial(water) });

            return scene;
        }

        /// A level bed `depth` units under a level surface of water, both `extent` across.
        ///
        /// The shape most of the water tests want: something to see through the water at, and the
        /// water to see it through.
        SceneDesc makeFlooded(float extent, float depth)
        {

            SceneDesc scene = makeOpenWater(extent);
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(extent, -depth), {}, {}, sQuadIndices) });

            return scene;
        }

        /// A bed that shelves: `depth` under the origin, and dropping away northward at `gradient`.
        ///
        /// **The shape a surf zone needs, and a level pan is not one.** Where foam sits is set by how
        /// far the wave came through water shallow enough to break in, and across a level bed that
        /// distance is unbounded — so anything measuring surf has to give the bed somewhere to go
        /// down to.
        ///
        /// The shore end is cut off exactly where the bed would surface, so that no part of it stands
        /// proud of the water to shade the point being measured. The seaward end runs the full
        /// extent, because the run-out probe goes looking that way for water deep enough to break
        /// in and a bed that stopped short would read as dry land.
        SceneDesc makeShelving(float extent, float depth, float gradient)
        {
            SceneDesc scene = makeOpenWater(extent);

            const float shore = -depth / gradient;
            const float deep = -depth - gradient * extent;
            const std::array<osg::Vec3f, 4> bed{
                osg::Vec3f(-extent, shore, 0.0f),
                osg::Vec3f(extent, shore, 0.0f),
                osg::Vec3f(extent, extent, deep),
                osg::Vec3f(-extent, extent, deep),
            };

            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(bed, {}, {}, sQuadIndices) });

            return scene;
        }

        /// How bright the sun is in the tests that measure through water.
        ///
        /// Named because their arithmetic uses it as well: the number the shader is handed and the
        /// number an expectation is computed from have to be one number, or the test quietly stops
        /// describing the shader.
        constexpr float sSunOverWater = 2.0f;

        /// Where the sun stands, from how far it is off the vertical. Its light travels back along
        /// this, which is the only sun vector the shader has.
        osg::Vec3f sunStandingAt(float zenith)
        {
            return osg::Vec3f(0.0f, -std::sin(zenith), std::cos(zenith));
        }

        /// Where the sun stands over the tests that measure through water — and it must not be
        /// straight up.
        ///
        /// Overhead, the sun's own mirror image lands exactly where a camera looking straight down
        /// sends its reflection ray, and a saturated disc of sun in the middle of the frame is not
        /// what these are measuring. Two degrees puts it twelve disc-widths clear of the reflection
        /// and costs the arithmetic a part in a thousand, which `throughFlatWater` accounts for
        /// anyway.
        const float sNearlyOverhead = osg::DegreesToRadians(2.0f);

        /// Puts `camera` over a flooded scene with nothing in its sky but the sun.
        ///
        /// The water level is the one `makeFlooded` builds to, and the sky is black — so apart from
        /// the sun's own disc, the two per cent that reflects off the surface reflects nothing.
        /// Every byte in the frame has then come up through the depth, which is what lets these
        /// tests name an exact value.
        void litThroughWater(Shaders::VisibilityConstants& camera, float zenith = sNearlyOverhead)
        {
            camera.mSunPosition = sunStandingAt(zenith);
            camera.mSunIrradiance = osg::Vec3f(sSunOverWater, sSunOverWater, sSunOverWater);
            camera.mSkyHorizon = osg::Vec3f();
            camera.mSkyZenith = osg::Vec3f();
            camera.mWaterLevel = 0.0f;
        }

        /// A square in the xz plane at y = 0, facing along -Y, four hundred units across.
        /// Four hundred units square in the XZ plane, which is larger than any frame here sees.
        const std::array<osg::Vec3f, 4> sWallQuad{
            osg::Vec3f(-200.0f, 0.0f, -200.0f),
            osg::Vec3f(200.0f, 0.0f, -200.0f),
            osg::Vec3f(200.0f, 0.0f, 200.0f),
            osg::Vec3f(-200.0f, 0.0f, 200.0f),
        };

        SceneDesc makeWall()
        {
            SceneDesc scene;
            const Index mesh = scene.addMesh(sWallQuad, {}, {}, sQuadIndices);
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh });
            return scene;
        }

        /// What the shader writes for a linear value, so a test can name the byte it expects.
        std::uint8_t encodeSrgb(float linear)
        {
            const float encoded
                = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
            return static_cast<std::uint8_t>(std::lround(std::clamp(encoded, 0.0f, 1.0f) * 255.0f));
        }

        /// A byte the shader wrote, back to the linear value behind it.
        ///
        /// Ratios have to be taken in linear. sRGB is a power curve, so the same proportional
        /// brightening is a different number of bytes at the top of the range and at the bottom.
        float decodeSrgb(std::uint8_t byte)
        {
            const float encoded = static_cast<float>(byte) / 255.0f;
            return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }

        /// What the centre pixel must read over `depth` units of flat water with a bed under it.
        ///
        /// The bed is untextured, so its albedo is 0.5, and **the water is crossed twice**: the sun
        /// is attenuated on its way down and again on the way back up to the eye, so what arrives is
        /// the product of two paths and not one of them. Lighting the bottom as though the water
        /// over it were not there is what makes the same column read differently from above and
        /// below.
        ///
        /// The way down is the *slant* path — Snell bends the sun to `asin(sin(zenith) / IOR)` and
        /// it crosses `depth / cos` of water — while the way back is vertical, because the camera
        /// is. The cosine on the bed stays the sun's own in air: refraction at a level surface moves
        /// no flux across a horizontal patch, so what lands on the bottom is what fell on the top
        /// times whatever the longer path took.
        ///
        /// Nothing else adds. The scattering term `(1 - T^2) / 2` meets a black ambient, and the
        /// sky is black, so all the surface reflects is the two per cent it takes off the way in.
        int throughFlatWater(float depth, float zenith, std::size_t channel)
        {
            const float sine = std::sin(zenith) / Shaders::WATER_IOR;
            const float refracted = std::sqrt(1.0f - sine * sine);

            const float down = std::exp(-Shaders::WATER_EXTINCTION[channel] * depth / refracted);
            const float up = std::exp(-Shaders::WATER_EXTINCTION[channel] * depth);
            const float bed = 0.5f * sSunOverWater * std::cos(zenith) * Shaders::INV_PI;

            return encodeSrgb(bed * down * up * (1.0f - Shaders::WATER_F0));
        }

        /// How brightly the sky is lit, and what colour the air is, in the tests that measure a
        /// wall through fog.
        ///
        /// Named for the reason `sSunOverWater` is: each test's arithmetic uses these as well as
        /// handing them to the shader, and the two have to be one number. The haze is deliberately
        /// not grey, so a fog scattering the wrong colour cannot pass by matching a total.
        ///
        /// **The sky rather than the cell's ambient, because a wall is lit by what it can see.** The
        /// ambient is what terminates a path now, one bounce further along; what fills a surface the
        /// eye is looking at is the hemisphere it gathers. A sky of one radiance makes that gather
        /// exact rather than noisy — every direction returns the same number, so one sample is the
        /// whole answer.
        constexpr float sFoggySky = 0.6f;
        const osg::Vec3f sHaze(0.1f, 0.2f, 0.4f);

        /// Puts `camera` under a sky of one radiance and nothing else, with air of `extinction` in it
        /// pooling at `level`.
        void litThroughFog(Shaders::VisibilityConstants& camera, float extinction,
            float level = -std::numeric_limits<float>::infinity())
        {
            camera.mSkyHorizon = osg::Vec3f(sFoggySky, sFoggySky, sFoggySky);
            camera.mSkyZenith = camera.mSkyHorizon;
            camera.mFogColour = sHaze;
            camera.mFogExtinction = extinction;
            camera.mWaterLevel = level;

            // Even air, which is what every exact expectation here needs: a banked field varies
            // along the ray, and then the march stops telescoping and its answer stops being one
            // anyone can write down. The banks have their own test.
            camera.mFogUniform = 1.0f;
        }

        /// A texture a test builds by hand, and the storage its description spans.
        ///
        /// Filled in place rather than returned, because `TextureData` carries spans into these
        /// vectors and nothing should have to reason about whether a move kept their buffers.
        struct TestTexture
        {
            std::vector<std::uint8_t> mBytes;
            std::vector<MipLevel> mLevels;
            TextureData mData;
        };

        /// A texture whose every mip is one flat colour — level `i` is `40 + 30i`, evenly spaced
        /// and none of them black.
        ///
        /// **The byte a ray comes back with reads out the level it sampled**, and because the levels
        /// are evenly spaced in value, `textureLod` blending two of them lands exactly on
        /// `40 + 30 * lod`. A *fractional* level is readable that way, which is what makes a cone's
        /// width measurable rather than merely orderable. Flat colours also mean the answer does not
        /// depend on where in the texture the cone landed.
        void makeMipLadder(TestTexture& texture)
        {
            constexpr std::uint32_t extent = 64;
            constexpr std::uint32_t levels = 7;

            for (std::uint32_t level = 0; level < levels; ++level)
            {
                const std::uint32_t side = extent >> level;
                texture.mLevels.push_back(MipLevel{ static_cast<std::uint32_t>(texture.mBytes.size()), side, side });
                texture.mBytes.insert(
                    texture.mBytes.end(), std::size_t{ side } * side * 4, static_cast<std::uint8_t>(40 + 30 * level));
            }

            texture.mData = TextureData{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = extent,
                .mHeight = extent,
                .mBytes = std::as_bytes(std::span(texture.mBytes)),
                .mLevels = texture.mLevels,
                .mName = "mip ladder",
            };
        }

        /// A texture that is white and wholly opaque, at one level.
        ///
        /// **What `makeMipLadder` cannot be.** Its levels encode which one was sampled, so its alpha
        /// is the level's own byte and its first is 40 of 255 — a sprite cut from it covers a sixth
        /// of what is behind it, which is a fine thing to be seen through and no use at all for
        /// asking what happens when a sprite owns a pixel.
        void makeOpaqueSheet(TestTexture& texture)
        {
            constexpr std::uint32_t extent = 4;

            texture.mLevels.push_back(MipLevel{ 0, extent, extent });
            texture.mBytes.assign(std::size_t{ extent } * extent * 4, std::uint8_t{ 255 });

            texture.mData = TextureData{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = extent,
                .mHeight = extent,
                .mBytes = std::as_bytes(std::span(texture.mBytes)),
                .mLevels = texture.mLevels,
                .mName = "opaque sheet",
            };
        }

        /// Which level of `makeMipLadder` a linear sample came from.
        float ladderLevel(float sampled)
        {
            return (sampled * 255.0f - 40.0f) / 30.0f;
        }

        /// The lobe the shader arrives at for a sea whose every wave is past the cone's reach.
        ///
        /// Read off the same table the shader reads. With no wave resolved, `unresolved` is the
        /// whole spectrum's mean square slope, and the lobe is twice its root — a normal tilted by
        /// an angle turns a reflection by twice it.
        float lobeOf(const SeaState& sea)
        {
            float unresolved = 0.0f;
            for (const Shaders::GpuWave& wave : sea.getWaves())
            {
                const float steepness = wave.mAmplitude * wave.mWavenumber;
                unresolved += 0.5f * steepness * steepness;
            }

            return std::min(2.0f * std::sqrt(unresolved), 1.0f);
        }

        class RtxVisibilityTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                std::string reason;
                mRenderer = Testing::getRenderer(reason);
                if (mRenderer == nullptr)
                    GTEST_SKIP() << reason;

                // Draining is how the slate is cleared: whatever a previous test left behind is not
                // this one's to report.
                mRenderer->takeValidationErrors(mErrors);
            }

            void TearDown() override
            {
                if (mRenderer == nullptr)
                    return;

                mRenderer->takeValidationErrors(mErrors);
                for (const std::string& error : mErrors)
                    ADD_FAILURE() << "validation error: " << error;
            }

            /// Renders `scene` at `size` square and returns how many primary rays hit.
            /// @param sea what the water is doing. A state with no height in it is a flat sea, which
            ///        is what a test asserting an exact transmittance through one needs.
            /// @param accumulate how many differently-seeded frames to average, overriding the
            ///        camera's own frame index with 0, 1, ... Zero renders the camera's frame alone.
            /// @param filter whether the denoiser runs, and off by default on purpose. Almost
            ///        everything here asserts a radiance a particular pixel must have, and a filter
            ///        mixes its neighbours into it — a test that let one run would be measuring the
            ///        denoiser rather than the thing it was written to measure. The tests that are
            ///        about the filter ask for it.
            std::uint32_t countHits(const SceneDesc& scene, std::span<const TextureData> textures,
                const Shaders::VisibilityConstants& camera, std::uint32_t size, std::vector<std::uint8_t>& pixels,
                const SeaState& sea = SeaState{}, std::uint32_t accumulate = 0, bool filter = false,
                bool jitter = false, std::optional<float> exposure = 1.0f)
            {
                mRenderer->resize(size, size);
                mRenderer->setScene(Rtx::sWorld, scene, inSceneOrder(textures), sea);

                // One frame per sample, where this used to record several dispatches into a single
                // submit. The renderer fences between frames, which orders them, and its own history
                // barrier is what makes each sum visible to the next.
                const std::uint32_t frames = std::max(accumulate, 1u);
                std::uint32_t hits = 0;
                for (std::uint32_t frame = 0; frame < frames; ++frame)
                {
                    Shaders::VisibilityConstants sampled = camera;
                    if (accumulate > 0)
                        sampled.mFrame = frame;

                    // Every frame hits the same primary geometry, so the last one's count is the
                    // answer rather than a sum to be divided back down.
                    hits = mRenderer
                               ->renderFrame(sampled,
                                   FrameOptions{ .mAccumulate = accumulate > 0 ? frame + 1 : 0,
                                       .mJitter = jitter,
                                       .mFilter = filter,
                                       .mExposure = exposure })
                               .mHits;
                }

                mRenderer->readPixels(pixels);
                return hits;
            }

            /// The same render as `countHits`, read back in linear radiance rather than as bytes.
            ///
            /// **What a figure is measured on.** `readPixels` gives the picture a display would
            /// show — eight bits, after the display curve — and the filter figures below had reached
            /// the point where that was the quantiser talking: two thirds of a byte at the
            /// brightness they sit at. `Channel::Radiance` is the same frame before either, and it
            /// is the *same quantity* while nothing tone-maps and the exposure is pinned at one, so
            /// the numbers stay comparable to the ones that were recorded through bytes.
            void renderRadiance(const SceneDesc& scene, const Shaders::VisibilityConstants& camera, std::uint32_t size,
                std::vector<float>& values, std::uint32_t accumulate = 0, bool filter = false)
            {
                // The bytes are read and thrown away: they are what sharing `countHits`'s render
                // loop costs, and one image copy against a sixty-four frame render is not a trade
                // worth a second copy of that loop.
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, SeaState{}, accumulate, filter);
                mRenderer->readChannel(Channel::Radiance, values);
            }

            /// A run of filtered frames with the history let build, read back in linear radiance.
            ///
            /// **Not `countHits` with a count on it, and the difference is the whole point.**
            /// `mAccumulate` averages finished frames in the composite; this leaves that at nought
            /// and advances `mFrame` instead, so each frame is a different draw and the only thing
            /// combining them is the accumulator under test. `resetHistory` first, because a
            /// one-frame baseline taken after a longer run is a baseline that already has a history
            /// in it — which reads as the accumulator doing nothing at all.
            void renderFiltered(const SceneDesc& scene, const Shaders::VisibilityConstants& camera, std::uint32_t size,
                std::vector<float>& values, std::uint32_t frames)
            {
                mRenderer->resize(size, size);
                mRenderer->setScene(Rtx::sWorld, scene, {}, SeaState{});
                mRenderer->resetHistory();

                for (std::uint32_t frame = 0; frame < frames; ++frame)
                {
                    Shaders::VisibilityConstants sampled = camera;
                    sampled.mFrame = frame;
                    mRenderer->renderFrame(
                        sampled, FrameOptions{ .mAccumulate = 0, .mFilter = true, .mExposure = 1.0f });
                }

                mRenderer->readChannel(Channel::Radiance, values);
            }

            /// The fixture's textures, numbered the way its scene added them.
            ///
            /// **A convention of these tests and not of the renderer.** Every test here builds its
            /// descriptions in the order its scene calls `addTexture`, so position is slot. The
            /// array used to assume that of every caller, which is a trap for the one whose scene
            /// has given a slot back: its table has a hole in it and its descriptions do not.
            std::span<const TextureData> inSceneOrder(std::span<const TextureData> textures)
            {
                mNumbered.assign(textures.begin(), textures.end());
                for (std::size_t at = 0; at < mNumbered.size(); ++at)
                    mNumbered[at].mSlot = static_cast<std::uint32_t>(at);

                return mNumbered;
            }

            Renderer* mRenderer = nullptr;
            std::vector<TextureData> mNumbered;
            std::vector<std::string> mErrors;
        };

        /// Halton, against its own definition worked out by hand.
        ///
        /// The radical inverse writes an index in a base and reflects its digits about the point, so
        /// term one in base two is 0.1 binary and term two is 0.01 — a half and a quarter. Base
        /// three's first three are a third, two thirds and a ninth. Centring subtracts a half from
        /// each, and the sequence is counted from one because term zero is the origin: a frame that
        /// sampled the pixel's corner would tell an upscaler nothing an unjittered one did not.
        TEST(RtxJitterTest, theSequenceIsHaltonInTwoAndThreeAndStraddlesTheCentre)
        {
            EXPECT_NEAR(haltonJitter(0).x(), 0.0f, 1e-6f) << "1/2 - 1/2";
            EXPECT_NEAR(haltonJitter(1).x(), -0.25f, 1e-6f) << "1/4 - 1/2";
            EXPECT_NEAR(haltonJitter(2).x(), 0.25f, 1e-6f) << "3/4 - 1/2";
            EXPECT_NEAR(haltonJitter(3).x(), -0.375f, 1e-6f) << "1/8 - 1/2";

            EXPECT_NEAR(haltonJitter(0).y(), 1.0f / 3.0f - 0.5f, 1e-6f);
            EXPECT_NEAR(haltonJitter(1).y(), 2.0f / 3.0f - 0.5f, 1e-6f);
            EXPECT_NEAR(haltonJitter(2).y(), 1.0f / 9.0f - 0.5f, 1e-6f);

            // Inside the pixel, every term, which is what makes it a sub-pixel offset rather than a
            // camera shake.
            for (std::uint32_t index = 0; index < 64; ++index)
            {
                const osg::Vec2f at = haltonJitter(index);
                EXPECT_GE(at.x(), -0.5f);
                EXPECT_LT(at.x(), 0.5f);
                EXPECT_GE(at.y(), -0.5f);
                EXPECT_LT(at.y(), 0.5f);
            }
        }

        /// Parallel rays, and the whole difference between them and a pinhole's.
        ///
        /// **The count is exact, so the arithmetic is the assertion.** A sheet fifty units across
        /// lies two hundred units under an eye looking straight down. The orthographic camera's box
        /// is two hundred across, so the sheet covers a quarter of each axis: pixel `p` of
        /// sixty-four samples the world at `100 * ((p + 0.5) / 32 - 1)`, which is inside twenty-five
        /// for `p` in 24..39 — sixteen columns, sixteen rows, **256 hits**, and no pixel near enough
        /// the boundary for rounding to argue.
        ///
        /// The same viewpoint as a pinhole with a ninety-degree field of view spans four hundred
        /// units at that distance rather than two hundred, so the sheet covers an eighth of each
        /// axis: `p` in 28..35, **64 hits**. That the two differ is the point — a parallel ray that
        /// quietly fanned out would still fill a plausible-looking image.
        /// A scene with nothing in it still binds a buffer for everything the shader declares.
        ///
        /// **A null handle at a descriptor is undefined at the dispatch**, and undefined here meant a
        /// lost device five seconds in, intermittently, with no message — the loop waited forever on
        /// a fence that would never signal. Three tables were doing it: a scene with no textures
        /// asked for no shading maps and a frame with no sprites asked for no tiles, and the rule
        /// every table was grown by read "grow if what is wanted does not fit", which never makes one
        /// at all when nothing is wanted.
        ///
        /// The fix is that the owner opens every table when it is built rather than when something
        /// writes to one, because the write is exactly what does not happen. This is the assertion
        /// that says so, and it is the one that would have caught it.
        TEST(RtxSceneTableTest, aSceneWithNothingInItStillBindsATableForEverythingDeclared)
        {
            std::string reason;
            Testing::Harness* harness = Testing::getHarness(reason);
            if (harness == nullptr)
                GTEST_SKIP() << reason;

            Device& device = *harness->mDevice;
            CommandPool pool(device);

            const SceneDesc empty;
            Batch setup(pool);

            // No textures, so no shading maps; no sprites, so no tiles. Both tables used to come out
            // as `VK_NULL_HANDLE`.
            const TextureArray textures(device, setup, 0, {});
            const SceneBuffers buffers(device, setup, empty, {}, SeaState{});
            setup.flush();

            EXPECT_NE(textures.getShading(), VK_NULL_HANDLE) << "the shading maps";

            // **Every table this hands out, and not the three that were caught.** The rule was the
            // same for all of them; which ones happened to be empty on the day is not what decides
            // whether they are covered.
            const std::array<std::pair<const char*, VkBuffer>, 16> tables{ {
                { "the normal blocks", buffers.getNormalBlocks() },
                { "the texture coordinate blocks", buffers.getTexCoordBlocks() },
                { "the meshes", buffers.getMeshes() },
                { "the instance rows", buffers.getInstances() },
                { "the materials", buffers.getMaterials() },
                { "the terrain layers", buffers.getLayers() },
                { "the blend masks", buffers.getMasks() },
                { "the lights", buffers.getLights() },
                { "the light grid's offsets", buffers.getLightOffsets() },
                { "the light grid's indices", buffers.getLightIndices() },
                { "the sprites", buffers.getSprites() },
                { "the emitters", buffers.getEmitters() },
                { "the sprite tile offsets", buffers.getSpriteTileOffsets() },
                { "the sprite tile indices", buffers.getSpriteTileIndices() },
                { "the light grid's geometry", buffers.getGrid() },
                { "the wave spectrum", buffers.getWaves() },
            } };

            for (const auto& [what, table] : tables)
                EXPECT_NE(table, VK_NULL_HANDLE) << what;
        }

        TEST_F(RtxVisibilityTest, anOrthographicCameraSendsItsRaysParallelRatherThanThroughAnEye)
        {
            constexpr std::uint32_t size = 64;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(25.0f, -100.0f), {}, {}, sQuadIndices) });

            // Straight down from a hundred units up, so the sheet is two hundred below the eye.
            // `lookAt` needs an up vector that is not the view direction; +Y is the map's own.
            const osg::Matrixf view
                = osg::Matrixf::lookAt(osg::Vec3f(0.0f, 0.0f, 100.0f), osg::Vec3f(), osg::Vec3f(0.0f, 1.0f, 0.0f));

            std::vector<std::uint8_t> pixels;

            const std::uint32_t parallel = countHits(scene, {},
                makeOrthographicCameraFromView(view, 200.0f, 200.0f, size, size, 1.0f, 10000.0f), size, pixels);

            const std::uint32_t pinhole
                = countHits(scene, {}, makeCameraFromView(view, 90.0f, size, size, 1.0f, 10000.0f), size, pixels);

            EXPECT_EQ(parallel, 16u * 16u);
            EXPECT_EQ(pinhole, 8u * 8u);
            EXPECT_NE(parallel, pinhole);
        }

        /// Which way the jitter moves the picture, which is the half of this that looks fine wrong.
        ///
        /// **A wrong sign still antialiases**, so nothing about a smoothed edge can catch one, and
        /// the reference implementation shipped both axes inverted. What catches it is an edge and a
        /// direction: a wall covering the left half of the frame, and a sample point moved right,
        /// has to lose exactly one column of hits.
        ///
        /// Half a pixel exactly, so the answer is a whole column and no pixel lands on the boundary.
        TEST_F(RtxVisibilityTest, theJitterMovesTheSampleTheWayTheImageIsIndexed)
        {
            constexpr std::uint32_t size = 64;

            // The frame is 2 * 100 * tan(30) = 115.47 units across at the wall, which is 1.8042 to
            // the pixel. The wall's edge is put a quarter of a pixel right of the image's centre
            // line — 0.4510 units — so that it falls between the boundary and the first column right
            // of it, and a half-pixel move takes exactly one column across it.
            constexpr float edge = 0.4510f;
            const std::array half{
                osg::Vec3f(-4000.0f, 0.0f, -4000.0f),
                osg::Vec3f(edge, 0.0f, -4000.0f),
                osg::Vec3f(edge, 0.0f, 4000.0f),
                osg::Vec3f(-4000.0f, 0.0f, 4000.0f),
            };

            SceneDesc scene;
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(half, {}, {}, sQuadIndices) });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            const auto covered = [&](float acrossX) {
                camera.mCamera.mJitter = osg::Vec2f(acrossX, 0.0f);

                std::vector<std::uint8_t> pixels;
                return countHits(scene, {}, camera, size, pixels);
            };

            const std::uint32_t centred = covered(0.0f);
            ASSERT_GT(centred, 0u);
            ASSERT_LT(centred, size * size) << "the wall has to cover part of the frame and not all";

            // **Asymmetric on purpose, because that is what carries the sign.** The first column
            // right of the edge samples a quarter pixel past it, so moving left by half a pixel
            // brings that column onto the wall and moving right by half a pixel changes nothing at
            // all. Invert either axis and the two swap.
            EXPECT_EQ(covered(-0.5f), centred + size) << "half a pixel left gains one column";
            EXPECT_EQ(covered(0.5f), centred) << "and half a pixel right crosses nothing";
        }

        /// Jitter and the reference mode together, which is the only thing jitter is good for.
        ///
        /// **One jittered frame is just a frame sampled slightly wrong.** What the sequence buys is
        /// what several of them cover between them: over sixteen frames the sample points spread
        /// across the pixel, so a pixel the edge cuts through averages the two sides in proportion
        /// to how much of it each covers. Unjittered, every frame samples the same point and the
        /// average is as hard-edged as one frame is.
        ///
        /// The edge is put a quarter of a pixel off the centre line, so the column it crosses is
        /// three quarters wall and one quarter sky and cannot come out as either.
        TEST_F(RtxVisibilityTest, jitteredFramesAverageIntoAnAntialiasedEdge)
        {
            constexpr std::uint32_t size = 64;
            constexpr float edge = 0.4510f;

            const std::array half{
                osg::Vec3f(-4000.0f, 0.0f, -4000.0f),
                osg::Vec3f(edge, 0.0f, -4000.0f),
                osg::Vec3f(edge, 0.0f, 4000.0f),
                osg::Vec3f(-4000.0f, 0.0f, 4000.0f),
            };

            SceneDesc scene;
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(half, {}, {}, sQuadIndices) });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            // Green sky, so the wall's grey and the sky cannot be confused, and a pixel that mixed
            // them reads as neither.
            camera.mSkyHorizon = osg::Vec3f(0.0f, 0.25f, 0.0f);
            camera.mSkyZenith = osg::Vec3f(0.0f, 0.25f, 0.0f);
            camera.mShowAlbedo = 1u;

            // The last column the wall covers, and the first one past it.
            constexpr std::size_t row = std::size_t{ size / 2 } * size;
            const auto redAt = [](const std::vector<std::uint8_t>& pixels, std::size_t column) {
                return static_cast<int>(pixels[(row + column) * 4]);
            };

            std::vector<std::uint8_t> hard;
            countHits(scene, {}, camera, size, hard, SeaState{}, 16, false, false);

            std::vector<std::uint8_t> soft;
            countHits(scene, {}, camera, size, soft, SeaState{}, 16, false, true);

            // Unjittered, every one of the sixteen samples the same point, so the two columns are
            // the wall's byte and the sky's with nothing between them.
            EXPECT_NEAR(redAt(hard, size / 2 - 1), 187, 1) << "wall";
            EXPECT_EQ(redAt(hard, size / 2), 0) << "sky, which has no red in it";

            // Jittered, the column the edge crosses is part of each. Red comes only from the wall,
            // so anything between nothing and the wall's own byte is the edge being resolved.
            EXPECT_NEAR(redAt(soft, size / 2 - 1), 187, 1) << "still wall a whole pixel in";
            EXPECT_GT(redAt(soft, size / 2), 10) << "the edge column picked up some wall";
            EXPECT_LT(redAt(soft, size / 2), 180) << "and did not become it";
        }

        /// Motion vectors, which can be plausible and wrong in three separate ways.
        ///
        /// So all three are asserted: a still camera leaves every pixel where it is; a camera that
        /// only *turns* moves a surface by an amount that does not depend on how far away it is; and
        /// a camera that *steps* moves a near surface further than a far one.
        ///
        /// **The same pixel at two depths, and not two pixels at one depth.** A perspective rotation
        /// is not a uniform slide — a point at the edge of the frame moves further than one at its
        /// centre, because screen position goes as the tangent of the angle. Comparing two places in
        /// one frame would measure that and call it a depth error, so each depth gets its own frame
        /// and the same pixel is read from both.
        ///
        /// The step's arithmetic. Moving the eye `s` sideways leaves the point now straight ahead
        /// standing `s` to the side of where the eye used to be, so its old screen position had
        /// `tan(angle) = s / d`. The basis carries `tan(30)` as its half width, so that is
        /// `(s / d) / tan(30)` in a coordinate running -1 to 1 across the frame, and 32 pixels to
        /// the unit over 64: `55.426 * s / d`. Four units at two hundred is 1.1085 pixels, at four
        /// hundred 0.5543.
        TEST_F(RtxVisibilityTest, aMotionVectorSaysWhereItsSurfaceWasAndNotWhereTheWorldIs)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::size_t centre = std::size_t{ size / 2 } * size + size / 2;

            /// A wall across the view at `away` units, large enough to fill the frame from anywhere
            /// these cameras stand.
            const auto wallAt = [](float away) {
                return std::array{
                    osg::Vec3f(-8000.0f, away, -8000.0f),
                    osg::Vec3f(8000.0f, away, -8000.0f),
                    osg::Vec3f(8000.0f, away, 8000.0f),
                    osg::Vec3f(-8000.0f, away, 8000.0f),
                };
            };

            /// The centre pixel's motion after the camera moves from `somewhere`, looking along +y,
            /// to `somewhere + eye` looking at `somewhere + at`.
            const auto motionFrom
                = [&](const osg::Vec3f& somewhere, float away, const osg::Vec3f& eye, const osg::Vec3f& at) {
                      SceneDesc scene;
                      std::array<osg::Vec3f, 4> wall = wallAt(away);
                      for (osg::Vec3f& corner : wall)
                          corner += somewhere;

                      scene.addInstance(MeshInstance{
                          .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(wall, {}, {}, sQuadIndices) });

                      const Shaders::VisibilityConstants first = makeCamera(
                          somewhere, somewhere + osg::Vec3f(0.0f, 100.0f, 0.0f), 60.0f, size, size, 1000000.0f);

                      std::vector<std::uint8_t> pixels;
                      EXPECT_EQ(countHits(scene, {}, first, size, pixels), size * size) << "at " << away;

                      mRenderer->renderFrame(
                          makeCamera(somewhere + eye, somewhere + at, 60.0f, size, size, 1000000.0f), FrameOptions{});

                      std::vector<float> motion;
                      mRenderer->readChannel(Channel::Motion, motion);
                      return osg::Vec2f(motion[centre * 2], motion[centre * 2 + 1]);
                  };

            /// The same, at the origin, where a formulation that subtracts world points still works.
            const auto motionAt = [&](float away, const osg::Vec3f& eye, const osg::Vec3f& at) {
                return motionFrom(osg::Vec3f(), away, eye, at);
            };

            // **A still camera.** An unproject followed by a project with a float rounding between
            // them, so this is not exactly zero and must not be far from it.
            {
                const osg::Vec2f held = motionAt(200.0f, osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f));

                EXPECT_NEAR(held.x(), 0.0f, 1e-3f) << "a frame that did not move";
                EXPECT_NEAR(held.y(), 0.0f, 1e-3f);
            }

            // **A still camera that jitters**, which is every frame an upscaler ever sees. Where in
            // its pixel a frame chose to sample says nothing about where the surface went, so this
            // is the same zero as above — and it is a separate case because the jitter is exactly
            // what a reprojection can leave in by accident: the ray that found the surface carries
            // the offset, and the pixel it is being compared against does not.
            //
            // Two different terms of the sequence, because a wrong answer that happened to be the
            // same both frames would still hold an upscaler's history in one wrong place rather
            // than shaking it between two.
            {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(wallAt(200.0f), {}, {}, sQuadIndices) });

                const Shaders::VisibilityConstants camera
                    = makeCamera(osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f), 60.0f, size, size, 1000000.0f);

                mRenderer->resize(size, size);
                mRenderer->setScene(Rtx::sWorld, scene, {}, SeaState{});

                for (const std::uint32_t frame : { 1u, 2u })
                {
                    Shaders::VisibilityConstants sampled = camera;
                    sampled.mFrame = frame;
                    mRenderer->renderFrame(sampled, FrameOptions{ .mJitter = true });
                }

                std::vector<float> motion;
                mRenderer->readChannel(Channel::Motion, motion);

                // A quarter pixel and better than a third: the second and third Halton terms, which
                // is what a reprojection that carried the jitter would report here.
                EXPECT_NEAR(motion[centre * 2], 0.0f, 1e-3f) << "a jittered frame that did not move";
                EXPECT_NEAR(motion[centre * 2 + 1], 0.0f, 1e-3f);
            }

            // **A camera that steps**, four units along +x. The point now straight ahead was to the
            // right of the old eye, so it comes back positive, and twice as far away halves it.
            {
                const float near = motionAt(200.0f, osg::Vec3f(4.0f, 0.0f, 0.0f), osg::Vec3f(4.0f, 100.0f, 0.0f)).x();
                const float far = motionAt(400.0f, osg::Vec3f(4.0f, 0.0f, 0.0f), osg::Vec3f(4.0f, 100.0f, 0.0f)).x();

                EXPECT_NEAR(near, 1.1085f, 0.02f) << "two hundred units away";
                EXPECT_NEAR(far, 0.5543f, 0.02f) << "and four hundred";
            }

            // **The same step, a hundred thousand units from the origin**, which is where Morrowind
            // actually is: the far corner of the map is past 200,000 and every cell but one is
            // somewhere out there.
            //
            // **The formulation keeps every device-side number small**, which is why the answer
            // out here is the same as the answer at the origin: the only subtraction of two world
            // points happens on the host, between two camera positions a step apart, and the device
            // adds that small delta to an offset from its own eye.
            //
            // Measured, and worth writing down: taking the difference on the device instead gives
            // bit-identical results at this distance, because the compiler folds `(o + x) - (o - m)`
            // back to `x + m`. So this asserts the answer rather than proving the formulation
            // necessary — what it would catch is a reprojection that built world-space clip
            // coordinates, whose intermediates really are six figures long.
            {
                const osg::Vec3f somewhere(100000.0f, 100000.0f, 0.0f);
                const float near
                    = motionFrom(somewhere, 200.0f, osg::Vec3f(4.0f, 0.0f, 0.0f), osg::Vec3f(4.0f, 100.0f, 0.0f)).x();

                EXPECT_NEAR(near, 1.1085f, 0.02f) << "the same two hundred units, a long way from the origin";
            }

            // **A camera that only turns**, about its own position and by the same angle whichever
            // wall it is looking at. Distance has no say in what a rotation does.
            {
                const osg::Vec3f turned(20.0f, 100.0f, 0.0f);
                const float near = motionAt(200.0f, osg::Vec3f(), turned).x();
                const float far = motionAt(400.0f, osg::Vec3f(), turned).x();

                EXPECT_GT(std::abs(near), 1.0f) << "the image slid";
                EXPECT_LT(std::abs(near), size) << "and stayed on screen";
                EXPECT_NEAR(near, far, 0.01f) << "by an amount its distance had no say in";
            }
        }

        /// The sky moves when the eye turns and stands still when it walks, and an upscaler is told
        /// which.
        ///
        /// **A miss used to store nothing at all**, on the reasoning that the sky does not move. That
        /// is true of walking and false of looking around, and looking around is most of what a
        /// player does — so the upscaler fetched the sky's history from the pixel it already
        /// occupied and every turn of the head smeared it. A gradient hides that; a field of stars
        /// does not, which is how it was found.
        ///
        /// **The claim is exact rather than approximate.** Under a rotation about the eye, where a
        /// point lands on screen depends on its direction and not on how far away it is — so the sky
        /// and a wall at any distance all move by the same number of pixels, and that is what makes
        /// this an equality and not a "something happened".
        TEST_F(RtxVisibilityTest, theSkyReprojectsByTheTurnAloneAndAWallAtAnyDistanceAgrees)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::size_t centre = std::size_t{ size / 2 } * size + size / 2;

            /// The centre pixel's motion after the camera moves from the origin, looking along +y, to
            /// `eye` looking at `at`, with a wall `away` units along that axis.
            ///
            /// **A negative `away` puts it behind the eye**, which is a frame of nothing but sky and
            /// is not the same thing as a scene with nothing in it — a renderer with no geometry at
            /// all has no acceleration structure to trace, and that is a different test.
            const auto motion = [&](float away, const osg::Vec3f& eye, const osg::Vec3f& at) {
                const std::array<osg::Vec3f, 4> wall{
                    osg::Vec3f(-8000.0f, away, -8000.0f),
                    osg::Vec3f(8000.0f, away, -8000.0f),
                    osg::Vec3f(8000.0f, away, 8000.0f),
                    osg::Vec3f(-8000.0f, away, 8000.0f),
                };

                SceneDesc scene;
                scene.addInstance(MeshInstance{
                    .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(wall, {}, {}, sQuadIndices) });

                const Shaders::VisibilityConstants first
                    = makeCamera(osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f), 60.0f, size, size, 1000000.0f);

                std::vector<std::uint8_t> pixels;
                const std::uint32_t hit = countHits(scene, {}, first, size, pixels);
                EXPECT_EQ(hit, away > 0.0f ? size * size : 0u) << "the frame is all wall or all sky";

                mRenderer->renderFrame(makeCamera(eye, at, 60.0f, size, size, 1000000.0f), FrameOptions{});

                std::vector<float> moved;
                mRenderer->readChannel(Channel::Motion, moved);
                return osg::Vec2f(moved[centre * 2], moved[centre * 2 + 1]);
            };

            // **A camera that only turns.** Twenty units across a hundred out is a little over eleven
            // degrees, and every one of these three lands on the same pixel offset.
            const osg::Vec3f turned(20.0f, 100.0f, 0.0f);
            const float sky = motion(-500.0f, osg::Vec3f(), turned).x();
            const float near = motion(200.0f, osg::Vec3f(), turned).x();
            const float far = motion(400.0f, osg::Vec3f(), turned).x();

            EXPECT_GT(std::abs(sky), 1.0f) << "the sky slid, which storing nothing could never say";
            EXPECT_LT(std::abs(sky), size) << "and stayed on screen";
            EXPECT_NEAR(sky, near, 0.01f) << "by exactly what a wall two hundred units off moved";
            EXPECT_NEAR(sky, far, 0.01f) << "and four hundred, because a turn does not care";

            // **A camera that only walks.** Now the distances part company and the sky is the one
            // that does not move: it is infinitely far, so a step sideways is nothing beside it.
            const osg::Vec3f aside(40.0f, 0.0f, 0.0f);
            const float walkedSky = motion(-500.0f, aside, aside + osg::Vec3f(0.0f, 100.0f, 0.0f)).x();
            const float walkedNear = motion(200.0f, aside, aside + osg::Vec3f(0.0f, 100.0f, 0.0f)).x();

            EXPECT_NEAR(walkedSky, 0.0f, 1e-3f) << "the sky is where it was";
            EXPECT_GT(std::abs(walkedNear), 1.0f) << "and the wall is not";

            // And a camera that did nothing moves nothing, sky included — an unproject and a project
            // with a rounding between them.
            EXPECT_NEAR(motion(-500.0f, osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f)).x(), 0.0f, 1e-3f);
        }

        /// The two answers the depth channel carries, against hand-computed values for both.
        ///
        /// **Clip depth in `r`, for an upscaler.** `far / (far - near) * (1 - near / z)`, zero at the
        /// near plane and one at the far one. The numbers here: near is 1 and far is 100,000, so a
        /// wall 200 units off reads `1.00001 * (1 - 1/200) = 0.995`, and one at 400 reads
        /// `1.00001 * (1 - 1/400) = 0.9975`. Most of the range is spent within a few units of the
        /// eye, which is exactly why the filter reads the second channel instead.
        ///
        /// **Distance from the eye in `g`, for the filter.** In world units, along the ray.
        ///
        /// **And the two disagree in the one place that matters**, which is what makes this a test
        /// rather than two readings of one number: at the corner of the frame the same plane is
        /// further away and no deeper. A 64-pixel square at a sixty-degree field of view puts pixel
        /// zero at `uv = 0.5/64 * 2 - 1 = -0.984375` on both axes, so its ray is
        /// `normalize(F - 0.984375 R + 0.984375 U)` with `|R| = |U| = tan(30°)`; the cosine to the
        /// view axis is `1 / sqrt(1 + 2 (0.984375 tan 30°)^2) = 1 / 1.2829652`. So the corner reads
        /// the centre's clip value and 1.2829652 times its distance. The centre pixel is itself half
        /// a pixel off-axis, which is the 1.0000814 below.
        TEST_F(RtxVisibilityTest, theDepthChannelIsWhatARasterizerWouldHaveWritten)
        {
            constexpr std::uint32_t size = 64;
            constexpr float far = 100000.0f;
            constexpr float near = 1.0f;

            const auto wallAt = [](float away) {
                return std::array{
                    osg::Vec3f(-8000.0f, away, -8000.0f),
                    osg::Vec3f(8000.0f, away, -8000.0f),
                    osg::Vec3f(8000.0f, away, 8000.0f),
                    osg::Vec3f(-8000.0f, away, 8000.0f),
                };
            };

            const auto expected = [](float z) { return far / (far - near) * (1.0f - near / z); };

            // Two floats a pixel: clip depth, then distance from the eye.
            constexpr std::size_t stride = 2;
            constexpr float cornerCosine = 1.2829652f;
            constexpr float centreCosine = 1.0000814f;

            const auto depthOf = [&](float away) {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(wallAt(away), {}, {}, sQuadIndices) });

                const Shaders::VisibilityConstants camera
                    = makeCamera(osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f), 60.0f, size, size, far);

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, {}, camera, size, pixels), size * size);

                std::vector<float> depth;
                mRenderer->readChannel(Channel::Depth, depth);
                return depth;
            };

            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * stride;
            constexpr std::size_t corner = 0;

            for (const float away : { 200.0f, 400.0f })
            {
                const std::vector<float> depth = depthOf(away);
                ASSERT_EQ(depth.size(), std::size_t{ size } * size * stride);

                EXPECT_NEAR(depth[centre], expected(away), 1e-5f) << "at " << away;

                // The corner sees the same plane, so it must read the same depth even though it is
                // a good deal further from the eye. Reading the ray's own length instead would put
                // this at `expected(away / cos)`, which at this field of view is a whole 0.00002
                // out — small, and exactly the kind of small that makes an upscaler shimmer.
                EXPECT_NEAR(depth[corner], depth[centre], 1e-6f) << "the corner of the same wall";

                // And the second channel is the reading the first is not: the corner really is
                // further away, by the cosine the depth deliberately divides out.
                EXPECT_NEAR(depth[centre + 1], away * centreCosine, away * 1e-3f) << "distance at the centre";
                EXPECT_NEAR(depth[corner + 1], away * cornerCosine, away * 1e-3f) << "distance at the corner";
            }

            // A ray that hit nothing is as far away as anything can be.
            {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(wallAt(200.0f), {}, {}, sQuadIndices) });

                const Shaders::VisibilityConstants away
                    = makeCamera(osg::Vec3f(), osg::Vec3f(0.0f, -100.0f, 0.0f), 60.0f, size, size, far);

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, {}, away, size, pixels), 0u);

                std::vector<float> depth;
                mRenderer->readChannel(Channel::Depth, depth);
                for (std::size_t i = 0; i < depth.size(); i += stride)
                {
                    ASSERT_EQ(depth[i], 1.0f) << "clip depth at " << i / stride;
                    ASSERT_EQ(depth[i + 1], far) << "distance at " << i / stride;
                }
            }
        }

        /// A mesh whose vertices changed is traced against the new ones, without a scene rebuild.
        ///
        /// **What a skinned body needs and moving an instance cannot give.** A crate that moves says
        /// so with its transform; an arm that swings does not — the actor's transform is where the
        /// actor stands, and the pose lives in vertices underneath it. So this wall stays at the
        /// identity throughout and only its four corners are written again: a `placeScene` that
        /// rebuilt the top level over an untouched bottom level would trace the first wall every
        /// time and read the first distance.
        ///
        /// The distance is the assertion rather than the hit count, because it names *where* the
        /// new triangles are and not merely that something changed. Its 1.0000814 is the centre
        /// pixel's own half-pixel offset from the view axis, worked out in the depth test above.
        TEST_F(RtxVisibilityTest, aDeformedMeshIsTracedAgainstItsNewVerticesWithoutRebuildingTheScene)
        {
            constexpr std::uint32_t size = 64;
            constexpr float far = 100000.0f;
            constexpr float centreCosine = 1.0000814f;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 2 + 1;

            const auto wallAt = [](float away) {
                return std::array{
                    osg::Vec3f(-8000.0f, away, -8000.0f),
                    osg::Vec3f(8000.0f, away, -8000.0f),
                    osg::Vec3f(8000.0f, away, 8000.0f),
                    osg::Vec3f(-8000.0f, away, 8000.0f),
                };
            };

            const Shaders::VisibilityConstants camera
                = makeCamera(osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f), 60.0f, size, size, far);

            SceneDesc scene;
            const Index wall = scene.addMesh(wallAt(200.0f), {}, {}, sQuadIndices);
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = wall });

            std::vector<std::uint8_t> pixels;
            ASSERT_EQ(countHits(scene, {}, camera, size, pixels), size * size);

            std::vector<float> depth;
            mRenderer->readChannel(Channel::Depth, depth);
            ASSERT_NEAR(depth[centre], 200.0f * centreCosine, 0.2f) << "where it was built";

            /// Writes the wall's corners again `away` units off and replaces the scene's placement,
            /// exactly as a frame of the game does: clear, re-walk, hand it back.
            const auto deformTo = [&](float away) {
                scene.clearPlacement();
                scene.updateMesh(wall, wallAt(away), {});
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = wall });
                mRenderer->placeScene(Rtx::sWorld, scene, SeaState{});
            };

            deformTo(400.0f);
            EXPECT_EQ(mRenderer->renderFrame(camera, FrameOptions{}).mHits, size * size);

            mRenderer->readChannel(Channel::Depth, depth);
            EXPECT_NEAR(depth[centre], 400.0f * centreCosine, 0.4f) << "and the structure followed its vertices";

            // Behind the eye, where a wall that was never rebuilt would still be filling the frame.
            deformTo(-1000.0f);
            EXPECT_EQ(mRenderer->renderFrame(camera, FrameOptions{}).mHits, 0u);
        }

        /// A wall bigger than the field of view leaves no room for sky.
        ///
        /// At a hundred units from a sixty-degree camera the frame is 2 * 100 * tan(30) = 115 units
        /// tall; the wall is four hundred. Every ray must land on it, so the answer is exact rather
        /// than a threshold.
        ///
        /// The colour is exact too. These quads carry no state set, so they get the untextured
        /// material: a linear albedo of 0.5, which the shader encodes on the way out as
        /// 1.055 * 0.5^(1/2.4) - 0.055 = 0.735, or 187 of 255.
        TEST_F(RtxVisibilityTest, aWallLargerThanTheFrameIsHitByEveryRay)
        {
            constexpr std::uint32_t size = 64;
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;

            std::vector<std::uint8_t> pixels;
            EXPECT_EQ(countHits(makeWall(), {}, camera, size, pixels), size * size);

            ASSERT_EQ(pixels.size(), std::size_t{ size } * size * 4);
            for (std::size_t i = 0; i < pixels.size(); i += 4)
            {
                ASSERT_NEAR(pixels[i], 187, 1) << "red at pixel " << i / 4;
                ASSERT_NEAR(pixels[i + 1], 187, 1) << "green at pixel " << i / 4;
                ASSERT_NEAR(pixels[i + 2], 187, 1) << "blue at pixel " << i / 4;
            }

            // **The same frame, measured rather than held, and the whole of the arithmetic is
            // here.** A flat frame is the one input whose exposure can be worked out by hand, and
            // working it out means going through the binning rather than around it — which is the
            // half a check against "it got darker" would not cover.
            //
            // Luminance is 0.5, so `log2` is -1 and the histogram places it at
            // `uint((-1 + 10) / 16 * 254) + 1 = 143`. Every lit pixel lands in that one bin, so the
            // mean bin is 143 and the reduction reads back
            // `(143 - 1) / 254 * 16 - 10 = -1.055118` — a luminance of 0.481258, which is the
            // quantisation and not a mistake. The key over that, to the adaptation power, is
            // `(0.18 / 0.481258)^0.75 = 0.478268`; the frame comes out at 0.239134 linear and
            // `1.055 * 0.239134^(1/2.4) - 0.055 = 0.5262`, or 134 of 255.
            std::vector<std::uint8_t> measured;
            EXPECT_EQ(countHits(makeWall(), {}, camera, size, measured, SeaState{}, 0, false, false, std::nullopt),
                size * size);

            ASSERT_EQ(measured.size(), pixels.size());
            for (std::size_t i = 0; i < measured.size(); i += 4)
            {
                ASSERT_NEAR(measured[i], 134, 1) << "red at pixel " << i / 4;
                ASSERT_NEAR(measured[i + 1], 134, 1) << "green at pixel " << i / 4;
                ASSERT_NEAR(measured[i + 2], 134, 1) << "blue at pixel " << i / 4;
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
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;

            constexpr std::array<std::uint8_t, 4> redTexel{ 255, 0, 0, 255 };
            constexpr std::array<std::uint8_t, 4> blueTexel{ 0, 0, 255, 255 };
            constexpr MipLevel one{ 0, 1, 1 };
            const auto describe = [&one](std::span<const std::uint8_t> texel, std::uint32_t slot) {
                return TextureData{
                    .mSlot = slot,
                    .mFormat = TextureFormat::Rgba8Unorm,
                    .mWidth = 1,
                    .mHeight = 1,
                    .mBytes = std::as_bytes(texel),
                    .mLevels = std::span(&one, 1),
                };
            };

            SceneDesc scene;
            const Index mesh = scene.addMesh(sWallQuad, {}, sQuadUv, sQuadIndices);
            const Index red
                = scene.addMaterial(Material{ .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("red.dds")) });
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = red });

            mRenderer->resize(size, size);
            const TextureData first = describe(redTexel, 0);
            mRenderer->setScene(Rtx::sWorld, scene, std::span(&first, 1), SeaState{});
            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });

            std::vector<std::uint8_t> shown;
            mRenderer->readPixels(shown);
            ASSERT_EQ(shown[centre], 255) << "the wall did not start out red";
            ASSERT_EQ(shown[centre + 2], 0);
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
            const TextureData second = describe(blueTexel, blueTexture);
            mRenderer->extendScene(Rtx::sWorld, scene, std::span(&second, 1), SeaState{});
            EXPECT_EQ(mRenderer->getTextureCount(Rtx::sWorld), 2u);

            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });
            mRenderer->readPixels(shown);

            // The nearer wall wears the texture that was appended, which is only true if its
            // descriptor went to element one. Written to element zero it would come out red, and
            // written nowhere it would come out as whatever the array holds there — both plausible.
            EXPECT_EQ(shown[centre], 0) << "red";
            EXPECT_EQ(shown[centre + 2], 255) << "blue";

            // And the first texture is still where it was: move the near wall out of the way and the
            // one behind it has to be red again, sampled from a descriptor nothing rewrote.
            scene.dropInstance(1);
            mRenderer->placeScene(Rtx::sWorld, scene, SeaState{});
            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });
            mRenderer->readPixels(shown);

            EXPECT_EQ(shown[centre], 255) << "the texture already uploaded was disturbed by the append";
            EXPECT_EQ(shown[centre + 2], 0);

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

            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = 1.0f });
            mRenderer->readPixels(shown);

            EXPECT_EQ(shown[centre], 255) << "the texture that survived lost its slot";
            EXPECT_EQ(shown[centre + 2], 0);

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

            EXPECT_EQ(shown[centre + 2], 255) << "the description landed at its position rather than its slot";
            EXPECT_EQ(shown[centre], 0);
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
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;

            // No textures at all, so the array is allocated with nothing in it. The untextured
            // material's 0.5 encoded, which is the 187 the first test in this file works out.
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
            constexpr MipLevel one{ 0, 1, 1 };
            const auto describe = [&one](std::span<const std::uint8_t> texel) {
                return TextureData{
                    .mFormat = TextureFormat::Rgba8Unorm,
                    .mWidth = 1,
                    .mHeight = 1,
                    .mBytes = std::as_bytes(texel),
                    .mLevels = std::span(&one, 1),
                };
            };
            const std::array<TextureData, 2> textures{ describe(redTexel), describe(greenTexel) };

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

        /// The other half of de-lighting: the shader dividing the estimate back out.
        ///
        /// `ShadingMap`'s own tests say the estimate is right; this says the frame uses it. A map is
        /// handed in rather than estimated, so what is asserted is the arithmetic at the sample and
        /// nothing about how the number was arrived at.
        ///
        /// The texture is a linear 128, which is 0.50196. Divided by a map of two that is 0.25098,
        /// and `1.055 * 0.25098^(1/2.4) - 0.055` encodes to 137 of 255; left alone it encodes to
        /// 188, which is the byte every untextured surface in this file comes out at for the same
        /// reason.
        TEST_F(RtxVisibilityTest, aTexturesPaintedLightIsDividedBackOutOfItsAlbedo)
        {
            constexpr std::uint32_t size = 32;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr std::size_t cells = std::size_t{ Shaders::SHADING_EXTENT } * Shaders::SHADING_EXTENT;

            constexpr std::array<std::uint8_t, 4> texel{ 128, 128, 128, 255 };
            constexpr MipLevel one{ 0, 1, 1 };

            std::array<float, cells> painted{};

            const TextureData grey{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = 1,
                .mHeight = 1,
                .mBytes = std::as_bytes(std::span(texel)),
                .mLevels = std::span(&one, 1),
                .mShading = painted,
            };

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
            constexpr float halfExtent = 57.735027f;

            TestTexture ladder;
            makeMipLadder(ladder);
            const std::span<const TextureData> textures(&ladder.mData, 1);

            const std::array positions{
                osg::Vec3f(-halfExtent, 0.0f, -halfExtent),
                osg::Vec3f(halfExtent, 0.0f, -halfExtent),
                osg::Vec3f(halfExtent, 0.0f, halfExtent),
                osg::Vec3f(-halfExtent, 0.0f, halfExtent),
            };

            SceneDesc scene;
            const Index mesh = scene.addMesh(positions, {}, sQuadUv, sQuadIndices);
            const Index material
                = scene.addMaterial(Material{ .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("mip.dds")) });
            scene.addInstance(
                MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

            const auto centreOf = [](const std::vector<std::uint8_t>& pixels) {
                return pixels[(std::size_t{ size / 2 } * size + size / 2) * 4];
            };

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
            constexpr float halfExtent = 57.735027f;

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

            const MipLevel one{ 0, 1, 1 };
            const MipLevel wide{ 0, size, 1 };
            const auto describe = [](TextureFormat format, std::uint32_t width, std::span<const std::uint8_t> bytes,
                                      const MipLevel& level) {
                return TextureData{
                    .mFormat = format,
                    .mWidth = width,
                    .mHeight = 1,
                    .mBytes = std::as_bytes(bytes),
                    .mLevels = std::span(&level, 1),
                };
            };

            const std::array<TextureData, 3> textures{
                describe(TextureFormat::Rgba8Unorm, 1, redTexel, one),
                describe(TextureFormat::Rgba8Unorm, 1, greenTexel, one),
                describe(TextureFormat::Rgba8Unorm, size, strip, wide),
            };

            const std::array positions{
                osg::Vec3f(-halfExtent, 0.0f, -halfExtent),
                osg::Vec3f(halfExtent, 0.0f, -halfExtent),
                osg::Vec3f(halfExtent, 0.0f, halfExtent),
                osg::Vec3f(-halfExtent, 0.0f, halfExtent),
            };
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

        /// The sun, which is one direction everywhere and casts a shadow to the end of the world.
        ///
        /// The wall's normal is (0, -1, 0), so a sun travelling straight along it meets it square and
        /// the whole answer is the irradiance: 0.5 albedo times 2.0 over pi is 0.318310 linear, which
        /// encodes to 1.055 * 0.318310^(1/2.4) - 0.055 = 0.599797, or 153 of 255.
        TEST_F(RtxVisibilityTest, theSunLightsWhatItFacesAndTheOccluderTakesItAway)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            const Shaders::VisibilityConstants base = makeCamera(
                osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            const std::array occluder{
                osg::Vec3f(-10.0f, -25.0f, -10.0f),
                osg::Vec3f(10.0f, -25.0f, -10.0f),
                osg::Vec3f(10.0f, -25.0f, 10.0f),
                osg::Vec3f(-10.0f, -25.0f, 10.0f),
            };

            // **A sky rather than the cell's ambient, because that is what fills a wall now.** The
            // ambient terminates a path one bounce further along; what a surface the eye can see
            // gathers is its own hemisphere, and a sky of one radiance makes that gather exact —
            // every direction returns the same number, so one sample is the whole answer.
            const auto render = [&](const osg::Vec3f& direction, const osg::Vec3f& irradiance, bool blocked,
                                    const osg::Vec3f& sky = osg::Vec3f()) {
                SceneDesc scene = makeWall();
                if (blocked)
                    scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(occluder, {}, {}, sQuadIndices) });

                Shaders::VisibilityConstants camera = base;
                camera.mSunPosition = -direction;
                camera.mSunIrradiance = irradiance;
                camera.mSkyHorizon = sky;
                camera.mSkyZenith = sky;

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);
                return pixels[centre];
            };

            // Travelling along +Y, which is straight into the wall's face.
            const osg::Vec3f onto(0.0f, 1.0f, 0.0f);
            const osg::Vec3f bright(2.0f, 2.0f, 2.0f);

            EXPECT_EQ(render(onto, bright, false), 153) << "square to the sun";
            EXPECT_EQ(render(onto, bright, true), 0) << "and with something standing in the way";

            // A sun travelling out of the wall rather than into it reaches its back, and is dropped
            // rather than arithmetically applied. Asserted against the sky, because a negative
            // contribution clamps to black as well and the two only tell apart against something:
            // 0.5 * 0.4 = 0.2 linear, which encodes to 124 of 255.
            EXPECT_EQ(render(-onto, bright, false, osg::Vec3f(0.4f, 0.4f, 0.4f)), 124)
                << "a sun behind the wall lights nothing";

            // Half the irradiance is nowhere near half the byte, because the encoding is not
            // linear: 0.5 * 1.0 / pi = 0.159155, which encodes to 0.435542, or 111 of 255.
            EXPECT_EQ(render(onto, osg::Vec3f(1.0f, 1.0f, 1.0f), false), 111) << "and half as much sun";

            // At sixty degrees off square the cosine is exactly a half, so this is the same radiance
            // the half-irradiance case gave — the same 111, reached the other way.
            const osg::Vec3f slanted(std::sqrt(3.0f) * 0.5f, 0.5f, 0.0f);
            EXPECT_EQ(render(slanted, bright, false), 111) << "or the same again from a slant";
        }

        /// A ray that hits nothing comes back with the sky the weather named, not a constant.
        TEST_F(RtxVisibilityTest, theSkyIsTheWeathersOwnColourAndRunsFromHorizonToZenith)
        {
            constexpr std::uint32_t size = 33;

            // Facing straight up, so the centre pixel looks at the zenith and the frame's edge looks
            // sixty degrees off it. Nothing is placed, so every ray misses.
            Shaders::VisibilityConstants camera
                = makeCamera(osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mSkyHorizon = osg::Vec3f(1.0f, 0.0f, 0.0f);
            camera.mSkyZenith = osg::Vec3f(0.0f, 0.0f, 1.0f);

            SceneDesc scene = makeWall();
            std::vector<std::uint8_t> pixels;
            countHits(scene, {}, camera, size, pixels);

            // The camera looks level, so the middle row's rays are horizontal: z of zero, which is
            // the horizon end of the mix exactly. Pure red, and no blue at all.
            const std::size_t middle = (std::size_t{ size / 2 } * size + size / 2) * 4;
            EXPECT_EQ(pixels[middle], 255) << "the horizon colour, undiluted";
            EXPECT_EQ(pixels[middle + 2], 0);

            // The top row tilts up by tan(30) of the half-frame, so its z is sin of that angle and
            // the mix has moved toward the zenith. Only the direction of the move is asserted: the
            // exact angle is the camera's business and has its own test.
            const std::size_t top = std::size_t{ size / 2 } * 4;
            EXPECT_LT(pixels[top], 255) << "less horizon overhead";
            EXPECT_GT(pixels[top + 2], 0) << "and some zenith";
        }

        /// A glow, which the engine treats as two different things and so does this.
        ///
        /// The emissive **colour** joins the light and is multiplied by the texture, so a surface
        /// glows *with its own texture in it*. The emissive **map** is added past the albedo, so it
        /// glows through whatever the surface is made of. Getting either backwards is visible: a
        /// mushroom cap comes out flat white, or a map on a coloured surface goes black.
        TEST_F(RtxVisibilityTest, aGlowJoinsTheLightAndAGlowingMapIsAddedPastIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float halfExtent = 57.735027f;

            const std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const std::array<std::uint8_t, 4> green{ 0, 255, 0, 255 };
            const std::array<std::uint8_t, 4> dimRed{ 64, 0, 0, 255 };

            const MipLevel one{ 0, 1, 1 };
            const auto describe = [&one](std::span<const std::uint8_t> bytes) {
                return TextureData{
                    .mFormat = TextureFormat::Rgba8Unorm,
                    .mWidth = 1,
                    .mHeight = 1,
                    .mBytes = std::as_bytes(bytes),
                    .mLevels = std::span(&one, 1),
                };
            };

            const std::array<TextureData, 3> textures{ describe(white), describe(green), describe(dimRed) };

            const std::array positions{
                osg::Vec3f(-halfExtent, 0.0f, -halfExtent),
                osg::Vec3f(halfExtent, 0.0f, -halfExtent),
                osg::Vec3f(halfExtent, 0.0f, halfExtent),
                osg::Vec3f(-halfExtent, 0.0f, halfExtent),
            };

            // Nothing lights this scene at all: no lamp, no sun, no ambient. Whatever comes back is
            // the surface's own glow and nothing else.
            const Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            const auto render = [&](Index diffuse, Index emissiveMap, const osg::Vec3f& emissiveColour) {
                SceneDesc scene;
                const Index mesh = scene.addMesh(positions, {}, sQuadUv, sQuadIndices);
                scene.addTexture(VFS::Path::NormalizedView("white.dds"));
                scene.addTexture(VFS::Path::NormalizedView("green.dds"));
                scene.addTexture(VFS::Path::NormalizedView("red.dds"));

                const Index material = scene.addMaterial(Material{
                    .mDiffuse = diffuse,
                    .mEmissive = emissiveMap,
                    .mEmissiveColour = emissiveColour,
                });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, textures, camera, size, pixels), size * size);
                return std::array<std::uint8_t, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // A quarter of a glow on a white surface. The scale carries the original's "one is a
            // fully lit surface" onto this renderer's, where the sun is eight and the sky a fifth of
            // that: 0.25 * 1.6 = 0.4 linear, and 1.055 * 0.4^(1/2.4) - 0.055 = 0.66514, or 170.
            const osg::Vec3f quarter(0.25f, 0.25f, 0.25f);
            EXPECT_EQ(render(0, sNoIndex, quarter)[0], 170) << "a glow on white";

            // The same glow on a texture with no red in it keeps the texture's colour, because the
            // glow goes through the albedo. Added past it, the surface would come back white.
            const std::array<std::uint8_t, 3> onGreen = render(1, sNoIndex, quarter);
            EXPECT_EQ(onGreen[1], 170) << "the same glow, still through green";
            EXPECT_EQ(onGreen[0], 0) << "and with none of the red the white one had";

            // The map is the other way round: red light off a green surface. Through the albedo it
            // would be black, since green times red is nothing. 64 of 255 is 0.25098, times 1.6 is
            // 0.40157, which encodes to 170 — the same byte, reached without the texture's help.
            const std::array<std::uint8_t, 3> mapped = render(1, 2, osg::Vec3f());
            EXPECT_EQ(mapped[0], 170) << "the map's own red, undimmed by the green under it";
            EXPECT_EQ(mapped[1], 0) << "and none of the green, which nothing is lighting";
        }

        /// A masked surface in front of a wall: the ray stops on what survives the cutout and goes
        /// on through what does not.
        ///
        /// The scene is arranged so the answer is a whole number of pixels. The mask is sixteen
        /// texels across, red throughout, and transparent over its left half; the quad carrying it
        /// exactly fills a sixty-degree frame at a hundred units, so image column c samples
        /// u = (c + 0.5) / 64 and the sampler's REPEAT wrap makes both seams behave the same way.
        /// At column 31 the filter sits 0.375 of the way onto the first opaque texel and at column
        /// 32 it sits 0.625 on, so a cutoff of 0.5 falls exactly between them with a margin of
        /// 0.125 either side — far wider than the four bits of sub-texel precision Vulkan
        /// guarantees. The left half of the image is therefore the wall behind and the right half
        /// is the mask, with nothing in between.
        TEST_F(RtxVisibilityTest, aCutoutStopsARayOnItsMaskAndLetsItThroughTheHoles)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::uint32_t extent = 16;
            constexpr std::uint32_t seam = size / 2;

            std::vector<std::uint8_t> bytes(std::size_t{ extent } * extent * 4);
            for (std::uint32_t y = 0; y < extent; ++y)
                for (std::uint32_t x = 0; x < extent; ++x)
                {
                    std::uint8_t* const texel = &bytes[(std::size_t{ y } * extent + x) * 4];
                    texel[0] = 255;
                    texel[3] = x < extent / 2 ? 0 : 255;
                }

            const MipLevel level{ 0, extent, extent };
            const TextureData data{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = extent,
                .mHeight = extent,
                .mBytes = std::as_bytes(std::span(bytes)),
                .mLevels = std::span(&level, 1),
            };

            const std::span<const TextureData> textures(&data, 1);

            // A hundred units from the camera, where the frame is 2 * 100 * tan(30) across.
            constexpr float halfExtent = 57.735027f;
            const std::array masked{
                osg::Vec3f(-halfExtent, -50.0f, -halfExtent),
                osg::Vec3f(halfExtent, -50.0f, -halfExtent),
                osg::Vec3f(halfExtent, -50.0f, halfExtent),
                osg::Vec3f(-halfExtent, -50.0f, halfExtent),
            };

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -150.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShowAlbedo = 1u;

            const auto render = [&](AlphaMode mode, float alphaRef) {
                SceneDesc scene = makeWall();
                const Index mesh = scene.addMesh(masked, {}, sQuadUv, sQuadIndices);
                const Index material = scene.addMaterial(Material{
                    .mDiffuse = scene.addTexture(VFS::Path::NormalizedView("mask.dds")),
                    .mAlphaRef = alphaRef,
                    .mAlphaMode = mode,
                });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

                std::vector<std::uint8_t> pixels;
                // Something is behind every hole, so every ray lands on one surface or the other.
                EXPECT_EQ(countHits(scene, textures, camera, size, pixels), size * size);
                return pixels;
            };

            // Red where the mask survived and grey where the wall shows through: the mask is pure
            // red, so its green is zero, and the untextured wall's albedo of 0.5 encodes to
            // 1.055 * 0.5^(1/2.4) - 0.055 = 0.73536, or 187.5 of 255 — which is why the grey is the
            // one value here given a byte of room.
            constexpr int wallGrey = 188;

            const std::vector<std::uint8_t> cutout = render(AlphaMode::Cutout, 0.5f);
            ASSERT_EQ(cutout.size(), std::size_t{ size } * size * 4);
            for (std::uint32_t row = 0; row < size; ++row)
                for (std::uint32_t column = 0; column < size; ++column)
                {
                    const std::uint8_t* const pixel = &cutout[(std::size_t{ row } * size + column) * 4];
                    if (column >= seam)
                    {
                        ASSERT_EQ(pixel[0], 255) << "red at " << column << ", " << row;
                        ASSERT_EQ(pixel[1], 0) << "green at " << column << ", " << row;
                    }
                    else
                    {
                        ASSERT_NEAR(pixel[0], wallGrey, 1) << "red at " << column << ", " << row;
                        ASSERT_NEAR(pixel[1], wallGrey, 1) << "green at " << column << ", " << row;
                    }
                }

            // A blend that named no threshold of its own is traced against the stand-in, and the
            // stand-in is the same half. Same bytes, or Morrowind's foliage — which is blended and
            // never alpha-tested — would not be cut out at all.
            EXPECT_EQ(render(AlphaMode::Blend, 0.0f), cutout);

            // And the control: the same texture on an opaque material hides the wall completely, so
            // it is the cutout doing this and not the geometry.
            const std::vector<std::uint8_t> opaque = render(AlphaMode::Opaque, 0.5f);
            for (std::size_t i = 0; i < opaque.size(); i += 4)
            {
                ASSERT_EQ(opaque[i], 255) << "red at pixel " << i / 4;
                ASSERT_EQ(opaque[i + 1], 0) << "green at pixel " << i / 4;
            }
        }

        /// A wall lit by one lamp, at the radiance the falloff says and nowhere else.
        ///
        /// The centre pixel looks straight at the origin, where the wall's normal is (0, -1, 0) and
        /// the light sits fifty units along it, so the cosine is exactly one and the whole answer is
        /// the falloff. Written out, with a reach of 500 and the untextured albedo of 0.5:
        ///
        ///   window    = 1 - (50 / 500)^4              = 0.99990
        ///   falloff   = window^2 / (50^2 + 1)         = 0.99980 / 2501 = 3.99760e-4
        ///   radiance  = 4000 * falloff / pi           = 0.508947
        ///   encoded   = 1.055 * (0.5 * 0.508947)^(1/2.4) - 0.055 = 0.54150, or 138 of 255
        ///
        /// The camera stands off the light's axis so that something can be put between the wall and
        /// the lamp without also standing in front of the wall.
        ///
        /// That cosine of one is also what pins the normal: it holds only if the plane's normal came
        /// back out of the acceleration structure unrotated and unmirrored, which symmetrical
        /// geometry otherwise hides well enough to survive being looked at.
        TEST_F(RtxVisibilityTest, aLampLightsAWallByItsFalloffAndAnObstacleTakesItAway)
        {
            // Odd, so one pixel sits exactly on the axis and its ray lands exactly on the origin.
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            const Shaders::VisibilityConstants base = makeCamera(
                osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            // Ten units across, a quarter of the way from the wall to the lamp: it covers the whole
            // shadow ray and none of the camera's, which passes through x = 25 at that height.
            const std::array occluder{
                osg::Vec3f(-10.0f, -25.0f, -10.0f),
                osg::Vec3f(10.0f, -25.0f, -10.0f),
                osg::Vec3f(10.0f, -25.0f, 10.0f),
                osg::Vec3f(-10.0f, -25.0f, 10.0f),
            };

            // A sky rather than the cell's ambient, for the reason the sun's own test gives: what
            // fills a wall the eye can see is the hemisphere it gathers.
            const auto render = [&](const std::optional<Light>& light, const osg::Vec3f& sky, bool blocked) {
                SceneDesc scene = makeWall();
                if (light.has_value())
                    scene.addLight(*light);
                if (blocked)
                    scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(occluder, {}, {}, sQuadIndices) });

                Shaders::VisibilityConstants camera = base;
                camera.mSkyHorizon = sky;
                camera.mSkyZenith = sky;

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);
                return pixels[centre];
            };

            const Light lamp{
                .mPosition = osg::Vec3f(0.0f, -50.0f, 0.0f),
                .mIntensity = osg::Vec3f(4000.0f, 4000.0f, 4000.0f),
                .mReach = 500.0f,
            };

            EXPECT_EQ(render(lamp, osg::Vec3f(), false), 138) << "lit by the lamp alone";

            // The same lamp with something in the way. Nothing else lights the wall, so the pixel
            // goes to black rather than merely dimmer — which is what tells a shadow from a falloff.
            EXPECT_EQ(render(lamp, osg::Vec3f(), true), 0) << "and shadowed by the quad between them";

            // Ambient with no lamp at all: 0.5 * 0.4 = 0.2 linear, which encodes to
            // 1.055 * 0.2^(1/2.4) - 0.055 = 0.48453, or 124 of 255.
            const osg::Vec3f sky(0.4f, 0.4f, 0.4f);
            EXPECT_EQ(render(std::nullopt, sky, false), 124) << "the sky alone";

            // A lamp behind the wall meets it at a cosine of minus one, and is dropped rather than
            // arithmetically applied. Asserted against the sky and not against black, because
            // black is what a negative contribution clamps to as well — the two only tell apart
            // where there is something for it to be subtracted from.
            Light behind = lamp;
            behind.mPosition = osg::Vec3f(0.0f, 50.0f, 0.0f);
            EXPECT_EQ(render(behind, sky, false), 124) << "a lamp on the far side lights nothing";

            // The window, biting at half the reach rather than at the very end of it, where it is
            // indistinguishable from the inverse square alone:
            //
            //   window   = 1 - (50 / 100)^4      = 0.93750
            //   falloff  = 0.87891 / 2501        = 3.51422e-4
            //   radiance = 4000 * falloff / pi   = 0.447465
            //   encoded  = 1.055 * (0.5 * 0.447465)^(1/2.4) - 0.055 = 0.51035, or 130 of 255
            Light near = lamp;
            near.mReach = 100.0f;
            EXPECT_EQ(render(near, osg::Vec3f(), false), 130) << "the window taking a bite out of it";

            // And the reach as a hard limit: at exactly its own reach a lamp is skipped outright.
            Light spent = lamp;
            spent.mReach = 50.0f;
            EXPECT_EQ(render(spent, osg::Vec3f(), false), 0) << "and one whose reach ends at the wall";
        }

        /// Which side of a surface the light may come from is the triangle's plane's answer, and a
        /// vertex normal that disagrees does not get to overrule it.
        ///
        /// **Morrowind's vertex normals point clean through their own triangles.** A stretch of the
        /// floor in the Seyda Neen customs office interpolates to one aimed at the ground, on a quad
        /// whose plane is level to a hundredth — and a normal merely turned to face the *ray* is
        /// left pointing down there, because at a shallow enough view it already does face the eye.
        /// A floor with its normal under it drops every lamp overhead on the cosine and sends its
        /// bounce into itself, which came out as a black band that slid about as the camera moved.
        ///
        /// The wall is met at fourteen degrees to its own plane, which is what makes that possible:
        /// the camera stands at `(200, -50, 0)`, so the ray travels `(-0.970, 0.243, 0)` and a normal
        /// of `(0.6, 0.8, 0)` — pointing through the wall, away from the lamp — still meets it at
        /// `-0.388` and passes for facing it.
        ///
        /// Three renders, and the arithmetic is the falloff from the lamp test with a cosine on it:
        ///
        ///   falloff  = (1 - (50 / 500)^4)^2 / (50^2 + 1)      = 3.99760e-4
        ///   plane    = 4000 * 0.5 * 1.0 * falloff / pi        = 0.254491 -> 138 of 255
        ///   tilted   = 4000 * 0.5 * 0.8 * falloff / pi        = 0.203593 -> 125 of 255
        ///
        /// The tilted normal is *used* — 125 is its own cosine of 0.8 and not the plane's one — and
        /// only which side it sits on is taken from the plane. Turned to face the ray instead it
        /// meets the lamp at minus 0.8 and the pixel is black, which is the whole of the defect.
        TEST_F(RtxVisibilityTest, aVertexNormalMayTiltAShadingModelButNotChooseWhichSideIsLit)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            // Fourteen degrees off the wall's own plane, and the centre ray lands exactly on the
            // origin — so the cosines below are the shading normal's and nothing else's.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(200.0f, -50.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mSkyHorizon = osg::Vec3f();
            camera.mSkyZenith = osg::Vec3f();

            const Light lamp{
                .mPosition = osg::Vec3f(0.0f, -50.0f, 0.0f),
                .mIntensity = osg::Vec3f(4000.0f, 4000.0f, 4000.0f),
                .mReach = 500.0f,
            };

            const auto render = [&](std::span<const osg::Vec3f> normals) {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(sWallQuad, normals, {}, sQuadIndices) });
                scene.addLight(lamp);

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);
                return pixels[centre];
            };

            // No vertex normals at all, so the plane is the whole answer and meets the lamp square.
            EXPECT_EQ(render({}), 138) << "the plane alone";

            // The same quad with a normal tilted through it, which is the case that was black.
            const osg::Vec3f through(0.6f, 0.8f, 0.0f);
            const std::array tilted{ through, through, through, through };
            EXPECT_EQ(render(tilted), 125) << "a normal authored through its own triangle still lights this side";

            // **And the authored side carries no meaning**, which is what makes the plane the only
            // thing deciding: negating every vertex normal is the same surface and has to be the
            // same pixel, to the byte.
            const std::array flipped{ -through, -through, -through, -through };
            EXPECT_EQ(render(flipped), render(tilted)) << "which way the normals were authored is not information";
        }

        /// A source with a size casts a penumbra, where a point casts an edge.
        ///
        /// **The whole of a soft shadow is where the shadow ray leaves from.** A lamp is a flame and
        /// the sun is a disc half a degree across, so neither is one direction: the ray is drawn
        /// from somewhere on the source, and the band an occluder hides some of it from is the
        /// penumbra. How wide that band is, is arithmetic — the source's own size seen from the
        /// occluder — and that is what this pins at both of its edges and in the middle.
        ///
        /// A half-plane occluder, so the geometry is one number: an edge at `x = X` in a plane
        /// parallel to the wall, hung behind it and so out of the camera's own view. The wall is at
        /// y = 0 and the source is straight out along -Y, so the **centre column** of pixels stands
        /// at x = 0 whatever row it is on — every one of them is at the same place in the penumbra,
        /// and averaging the column is averaging draws of one quantity rather than smearing several.
        /// Each is divided by what the same pixel reads with nothing in the way, so the falloff and
        /// the cosine — which do differ down the column — cancel and what is left is visibility.
        ///
        /// **The lamp** stands 400 units out with a source radius of 20, and the occluder hangs
        /// halfway: a ray leaving a point 20 units off the axis crosses the occluder's plane 10 off
        /// it, so an edge twelve units either side is wholly clear of the cone or wholly across it.
        ///
        ///     half-width = 200 * tan(asin(20 / 400)) = 10.013
        ///
        /// **The sun** is one direction everywhere, so its penumbra grows with nothing but the
        /// occluder's distance — two thousand units of it, at the disc's own half degree.
        ///
        ///     half-width = 2000 * tan(0.004654) = 9.308
        ///
        /// **And the same lamp with no size at all is the edge this replaced.** At `X = -1` it is
        /// still fully lit and at `X = +1` fully dark, where the sized one is part-lit at both — the
        /// segment of a disc of radius 10.013 cut one unit off its centre:
        ///
        ///     u        = -1 / 10.013                          = -0.09987
        ///     lit      = (acos(u) - u * sqrt(1 - u^2)) / pi    = 0.56348
        TEST_F(RtxVisibilityTest, aSourceWithASizeCastsAPenumbraAndAPointSourceCastsAnEdge)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::uint32_t column = size / 2;

            // Straight on, so the centre column's rays stay in the plane x = 0 and land on the wall
            // there, which is what makes one column a run of draws of the same quantity.
            const Shaders::VisibilityConstants base
                = makeCamera(osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(), 60.0f, size, size, 10000.0f);

            // A quad behind the wall covering everything left of `edge`, wide enough that no shadow
            // ray this frame sends leaves it by the far side.
            const auto halfPlane = [](float depth, float edge) {
                return std::array{
                    osg::Vec3f(edge - 1000.0f, depth, -1000.0f),
                    osg::Vec3f(edge, depth, -1000.0f),
                    osg::Vec3f(edge, depth, 1000.0f),
                    osg::Vec3f(edge - 1000.0f, depth, 1000.0f),
                };
            };

            const auto sceneWith = [&](const std::optional<Light>& lamp, float depth, std::optional<float> edge) {
                SceneDesc scene = makeWall();
                if (lamp.has_value())
                    scene.addLight(*lamp);
                if (edge.has_value())
                {
                    const std::array quad = halfPlane(depth, *edge);
                    scene.addInstance(MeshInstance{
                        .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(quad, {}, {}, sQuadIndices) });
                }
                return scene;
            };

            // **One frame where the answer is the same on every frame, and sixty-four where it is a
            // draw.** A pixel wholly inside or wholly outside the penumbra is decided by geometry
            // and repeats; one in the middle of it is a coin, and the mean of the column over
            // sixty-four frames is 33 * 64 = 2112 of them — a standard error of
            // sqrt(0.25 / 2112) = 0.0109, so the tolerance below is four and a half of them.
            const auto visible = [&](const SceneDesc& scene, const std::vector<float>& open,
                                     const Shaders::VisibilityConstants& camera, std::uint32_t frames) {
                std::vector<float> shadowed;
                renderRadiance(scene, camera, size, shadowed, frames, false);

                double total = 0.0;
                for (std::uint32_t row = 0; row < size; ++row)
                {
                    const std::size_t at = (std::size_t{ row } * size + column) * 4;
                    total += static_cast<double>(shadowed[at] / open[at]);
                }
                return static_cast<float>(total / size);
            };

            constexpr float lampDepth = -200.0f;

            // Bright enough to read well clear of the quantiser and nowhere near saturating: the
            // falloff at 400 units is 1 / 160001, so the wall comes back at 400000 / (160001 * pi)
            // times its own albedo of a half, which is 0.398.
            const Light lamp{
                .mPosition = osg::Vec3f(0.0f, -400.0f, 0.0f),
                .mIntensity = osg::Vec3f(400000.0f, 400000.0f, 400000.0f),
                .mReach = 10000.0f,
                .mRadius = 20.0f,
            };

            Shaders::VisibilityConstants lampCamera = base;
            lampCamera.mSkyHorizon = osg::Vec3f();
            lampCamera.mSkyZenith = osg::Vec3f();

            std::vector<float> lampOpen;
            renderRadiance(sceneWith(lamp, lampDepth, std::nullopt), lampCamera, size, lampOpen, 1, false);
            ASSERT_GT(lampOpen[(std::size_t{ column } * size + column) * 4], 0.0f) << "the lamp lights the wall";

            EXPECT_FLOAT_EQ(visible(sceneWith(lamp, lampDepth, -12.0f), lampOpen, lampCamera, 1), 1.0f)
                << "the whole source clears an edge outside its penumbra";
            EXPECT_FLOAT_EQ(visible(sceneWith(lamp, lampDepth, 12.0f), lampOpen, lampCamera, 1), 0.0f)
                << "and none of it clears one across the far side";
            EXPECT_NEAR(visible(sceneWith(lamp, lampDepth, 0.0f), lampOpen, lampCamera, 64), 0.5f, 0.05f)
                << "and exactly half of it stands on the shadow's own edge";

            // The same lamp with no size at all: the edge this replaced, crossing from wholly lit to
            // wholly dark inside the two units the sized one is still part-lit across.
            Light point = lamp;
            point.mRadius = 0.0f;

            EXPECT_FLOAT_EQ(visible(sceneWith(point, lampDepth, -1.0f), lampOpen, lampCamera, 1), 1.0f)
                << "a point source is lit right up to its shadow";
            EXPECT_FLOAT_EQ(visible(sceneWith(point, lampDepth, 1.0f), lampOpen, lampCamera, 1), 0.0f)
                << "and dark from there on, with no band in between";
            EXPECT_NEAR(visible(sceneWith(lamp, lampDepth, -1.0f), lampOpen, lampCamera, 64), 0.56348f, 0.05f)
                << "where a source with a size is still inside its own penumbra at both";

            // The sun, whose disc is the one this renderer draws and so the one it shadows by.
            constexpr float sunDepth = -2000.0f;

            Shaders::VisibilityConstants sunCamera = base;
            sunCamera.mSkyHorizon = osg::Vec3f();
            sunCamera.mSkyZenith = osg::Vec3f();
            // The disc stands along -Y, so its light travels +Y and meets the wall's face square.
            sunCamera.mSunPosition = osg::Vec3f(0.0f, -1.0f, 0.0f);
            sunCamera.mSunIrradiance = osg::Vec3f(2.0f, 2.0f, 2.0f);

            std::vector<float> sunOpen;
            renderRadiance(sceneWith(std::nullopt, sunDepth, std::nullopt), sunCamera, size, sunOpen, 1, false);
            ASSERT_GT(sunOpen[(std::size_t{ column } * size + column) * 4], 0.0f) << "the sun lights the wall";

            EXPECT_FLOAT_EQ(visible(sceneWith(std::nullopt, sunDepth, -10.0f), sunOpen, sunCamera, 1), 1.0f)
                << "the whole disc clears an edge outside its penumbra";
            EXPECT_FLOAT_EQ(visible(sceneWith(std::nullopt, sunDepth, 10.0f), sunOpen, sunCamera, 1), 0.0f)
                << "and none of it clears one across the far side";
            EXPECT_NEAR(visible(sceneWith(std::nullopt, sunDepth, 0.0f), sunOpen, sunCamera, 64), 0.5f, 0.05f)
                << "and half a disc stands on the shadow's own edge, two thousand units back";
        }

        /// A bounce is drawn by the cosine, and two thirds is the number that says so.
        ///
        /// **The one property of the estimator a uniform sky cannot show.** Every other test here
        /// fills the sky with one radiance so that a single sample carries no variance — which is
        /// what makes them exact, and what leaves `cosineDirection` unmeasured. A sky that runs from
        /// horizon to zenith turns the direction itself into the answer.
        ///
        /// Malley's method draws `d.z = sqrt(1 - u)` for uniform `u`, so
        ///
        ///   E[d.z]   = integral of sqrt(t) over [0, 1]  = 2/3
        ///   Var[d.z] = E[1 - u] - (2/3)^2 = 1/2 - 4/9   = 1/18,  sd = 0.235702
        ///
        /// A floor of albedo 0.5 under `mix(horizon, zenith, d.z)` therefore has to come back at
        /// `0.5 * (horizon + 2/3 * range)` per channel, spread by `0.5 * |range| * 0.235702`.
        ///
        /// **The mean is also what tells this estimator from the wrong one.** Drawing uniformly over
        /// the hemisphere and carrying the cosine as a weight is unbiased as well, but its
        /// directions average `E[d.z] = 1/2` — a picture a sixth of the sky's range away, which is
        /// ten times the tolerance below and cannot be mistaken for it.
        TEST_F(RtxVisibilityTest, aBounceDrawsItsDirectionByTheCosineAndNotUniformly)
        {
            constexpr std::uint32_t size = 64;
            constexpr float samples = float{ size } * size;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(4000.0f, 0.0f), {}, {}, sQuadIndices) });

            // Three ranges rather than one, and one of them descending, so a test that passed by
            // matching a total or by swapping two channels would not.
            const osg::Vec3f horizon(0.20f, 0.15f, 0.60f);
            const osg::Vec3f zenith(0.80f, 0.65f, 0.15f);

            // As near straight down as `makeCamera` will take, so every ray lands on the floor and
            // every pixel is one bounce off a normal of +z with nothing else in the frame. Nothing
            // stands above the floor either, so every bounce escapes and the sky is the whole answer.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, 300.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
            camera.mSkyHorizon = horizon;
            camera.mSkyZenith = zenith;

            const auto shade = [&](std::uint32_t frame, std::uint32_t accumulate = 0) {
                camera.mFrame = frame;

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, {}, camera, size, pixels, SeaState{}, accumulate), size * size);
                return pixels;
            };

            // The mean and the standard deviation of one channel across the frame, in linear.
            const auto measure = [&](const std::vector<std::uint8_t>& pixels, std::size_t channel) {
                float sum = 0.0f;
                float squares = 0.0f;
                for (std::size_t i = channel; i < pixels.size(); i += 4)
                {
                    const float linear = decodeSrgb(pixels[i]);
                    sum += linear;
                    squares += linear * linear;
                }

                const float mean = sum / samples;
                return std::pair{ mean, std::sqrt(squares / samples - mean * mean) };
            };

            const std::vector<std::uint8_t> first = shade(0);
            ASSERT_EQ(first.size(), std::size_t{ size } * size * 4);

            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto [mean, spread] = measure(first, channel);
                const float range = zenith[channel] - horizon[channel];
                const float byTheCosine = 0.5f * (horizon[channel] + range * 2.0f / 3.0f);
                const float ifDrawnEvenly = 0.5f * (horizon[channel] + range * 0.5f);

                // One sRGB step at a quarter brightness is 0.004 of linear — a 255th divided by the
                // curve's slope there — and it swamps the sampling standard error, which over 4096
                // samples is `0.5 * |range| * 0.235702 / 64`, at most 0.0011.
                EXPECT_NEAR(mean, byTheCosine, 0.004f) << "channel " << channel << " averages two thirds up";
                EXPECT_GT(std::abs(mean - ifDrawnEvenly), 0.02f)
                    << "channel " << channel << " is nowhere near the half an even draw would give";

                EXPECT_NEAR(spread, 0.5f * std::abs(range) * 0.235702f, 0.003f)
                    << "channel " << channel << " is spread by the square root's own variance";
            }

            // The frame index has to move every pixel's draw, or a bounce would be a fixed pattern
            // that no amount of accumulation could average away. Two independent samples land on the
            // same byte only by coincidence — green's range covers some eighty of them here, so a
            // few per cent — and the rest must differ.
            const std::vector<std::uint8_t> second = shade(1);
            std::size_t moved = 0;
            for (std::size_t i = 1; i < first.size(); i += 4)
                moved += first[i] != second[i] ? 1u : 0u;

            EXPECT_GT(moved, std::size_t{ size } * size * 9 / 10) << "the frame redraws the bounce";

            // **And what the accumulator is for: the error falls as the square root of the count.**
            // Averaging sixty-four independent draws divides the standard deviation of each by eight
            // and leaves the mean where it was — the whole basis for calling a long run a reference,
            // and worth asserting rather than assuming, because a sum that dropped or double-counted
            // a frame would still look converged.
            //
            // **And what the accumulator is for: averaging drives the error down and leaves the mean
            // alone.** Every pixel here has the same normal under the same sky, so its true value is
            // the same number — which makes the spread across the frame the error itself, and its
            // fall with the count the whole basis for calling a long run a reference.
            //
            // **The reduction is bounded at both ends, and both ends are derived.** Independent
            // draws would divide the standard deviation by `sqrt(64)`, which is eight, and nothing
            // can do worse than that — so a floor of eight catches a sum that dropped frames or a
            // turn that stopped turning, either of which leaves a pixel's samples repeating and the
            // spread where one frame left it. The ceiling is sixty-four, the reduction a perfectly
            // stratified sequence would reach, and it catches the opposite fault: a tile that failed
            // to upload reads as zero everywhere, every pixel draws the same direction as every
            // other, and the spread collapses to nothing while the mean stays right.
            //
            // Measured, the reduction is 17, 27 and 21 — comfortably past what independence gives,
            // because the frames are a golden-ratio sweep of the interval rather than sixty-four
            // guesses at it.
            //
            // **The mean's tolerance is twenty times tighter than the single-frame one above**, and
            // has to be: at sixty-four samples a pixel's values cluster inside a few bytes, so the
            // sRGB step that dominated there is no longer what limits this. A divisor off by one
            // moves the mean by 0.0046 — the whole point of the assertion, and something a tolerance
            // sized for one noisy frame would wave through.
            constexpr std::uint32_t averaged = 64;
            const std::vector<std::uint8_t> converged = shade(0, averaged);

            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto [mean, spread] = measure(converged, channel);
                const float range = zenith[channel] - horizon[channel];

                EXPECT_NEAR(mean, 0.5f * (horizon[channel] + range * 2.0f / 3.0f), 0.0002f)
                    << "channel " << channel << " keeps its mean when averaged";

                const float alone = 0.5f * std::abs(range) * 0.235702f;
                EXPECT_GT(alone / spread, std::sqrt(float{ averaged }))
                    << "channel " << channel << " converges at least as fast as independent draws";
                EXPECT_LT(alone / spread, float{ averaged })
                    << "channel " << channel << " converges no faster than a perfect sweep";
            }
        }

        /// Water is seen by a camera and not by a shadow ray, and the mask is what says so.
        ///
        /// Sunlight reaching a seabed has come through the surface, so a sea that occluded would
        /// black out every shallow in the game. Telling traversal in the mask costs nothing; the
        /// alternative — non-opaque water and a candidate loop that waves shadow rays past — was
        /// measured at half the frame rate, because every shadow ray crossing the sea then invokes a
        /// shader where traversal alone had been enough.
        TEST_F(RtxVisibilityTest, waterIsVisibleToACameraAndInvisibleToAShadowRay)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            // A pane twenty units across at y = -50, between the wall and everything in front of it.
            const std::array pane{
                osg::Vec3f(-20.0f, -50.0f, -20.0f),
                osg::Vec3f(20.0f, -50.0f, -20.0f),
                osg::Vec3f(20.0f, -50.0f, 20.0f),
                osg::Vec3f(-20.0f, -50.0f, 20.0f),
            };

            const auto renderPixel = [&](MaterialKind kind, bool lookAtIt) {
                SceneDesc scene = makeWall();

                Material material;
                material.mKind = kind;
                scene.addInstance(MeshInstance{
                    .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(pane, {}, {}, sQuadIndices),
                    .mMaterial = scene.addMaterial(material),
                });

                // Off the sun's axis, so the centre pixel lands on the patch of wall the pane
                // shadows without the camera's own ray having to cross the pane: it passes y = -50
                // at x = 50, and the pane reaches 20. Or straight at the pane, to see it at all.
                Shaders::VisibilityConstants camera = lookAtIt
                    ? makeCamera(
                          osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, -50.0f, 0.0f), 60.0f, size, size, 10000.0f)
                    : makeCamera(
                          osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
                camera.mSunPosition = osg::Vec3f(0.0f, -1.0f, 0.0f);
                camera.mSunIrradiance = osg::Vec3f(2.0f, 2.0f, 2.0f);

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);
                return std::array<std::uint8_t, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };
            const auto render = [&](MaterialKind kind, bool lookAtIt) { return renderPixel(kind, lookAtIt)[0]; };

            // A solid pane stops the camera's ray and the sun's alike, so the wall behind is dark.
            EXPECT_EQ(render(MaterialKind::Surface, false), 0) << "a solid pane shadows the wall";

            // The same pane as water: the camera still meets it, and the sun goes straight through.
            // 0.5 albedo times 2.0 over pi is 0.318310, which encodes to 153 of 255.
            EXPECT_EQ(render(MaterialKind::Water, false), 153) << "and water does not";

            // And the camera does still meet it. Asserted as "bluer than it is red", which the wall
            // cannot be at any brightness — it is grey through every channel — and which survives
            // water's shading changing, as it will.
            const std::array<std::uint8_t, 3> seen = renderPixel(MaterialKind::Water, true);
            EXPECT_GT(seen[2], seen[0]) << "though a camera still sees it";
        }

        /// Water over a bed, absorbing exactly what Beer-Lambert says it should over the path taken.
        ///
        /// A flat sea, so the surface is its own plane and the ray crosses the depth once rather than
        /// through whatever facet a wave put in the way. Nearly straight down — `makeCamera` will not
        /// take a look along the world's up axis, so this is a fifth of a degree off it, where the
        /// cosine is 0.99999 and Fresnel is its head-on 0.02.
        ///
        /// **The sun's path is not the depth unless the sun is overhead**, which is the second half
        /// of this: it enters at Snell's angle and crosses more water than it would straight down.
        ///
        /// Every expectation here derives from `WATER_EXTINCTION`, so a tuning pass is one line
        /// rather than five pieces of arithmetic that quietly stop describing the shader.
        TEST_F(RtxVisibilityTest, waterTakesWhatBeerLambertSaysOverThePathTheLightTook)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float depth = 200.0f;

            // A bed and a surface, both level and both wide enough to fill the frame.
            const SceneDesc scene = makeFlooded(400.0f, depth);

            const auto look = [&](float zenith) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -1.0f, 400.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
                litThroughWater(camera, zenith);

                // No height at all, which is a flat sea: a table whose amplitudes are zero. It is
                // also what makes the caustic exactly one — a flat surface has no curvature to
                // gather anything with, so the Jacobian is the identity.
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, SeaState{ .mSignificantHeight = 0.0f });
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // A sun as near overhead as the reflection allows, where `throughFlatWater`'s slant is
            // a part in three thousand and the answer is all but exactly `bed * T^2`:
            //
            //   0.5 * 2.0 / pi = 0.318310, times the two transmittances, times the 0.98 that is not
            //   the surface's own reflection.
            const std::array<int, 3> overhead = look(sNearlyOverhead);
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(overhead[channel], throughFlatWater(depth, sNearlyOverhead, channel), 1)
                    << "channel " << channel << " under a sun all but overhead";

            // And the ordering that makes this water's colour. Red goes first — every water
            // absorbs it within a metre or two, which is the one thing about water's colour that is
            // not a matter of taste. **Green survives most, not blue**, which is what makes a
            // coastal shelf green where open ocean is blue: Jerlov's coastal extinction is
            // 0.004572, 0.000714, 0.001143, so blue is taken half again as fast as green.
            EXPECT_LT(overhead[0], overhead[2]) << "red is taken before blue";
            EXPECT_LT(overhead[2], overhead[1]) << "and blue before green";

            // And now 45 degrees off the vertical, where the slant is the whole point. Snell's law
            // turns the sun to `asin(sin(45) / 1.333)` = 32.03 degrees, so it reaches a bed 200
            // units down after 200 / cos(32.03) = 235.93 units of water rather than 200 — while the
            // view back up is unchanged. One expectation serves both angles, which is what makes
            // this a measurement of the zenith rather than two unrelated numbers.
            const float slanted = osg::DegreesToRadians(45.0f);
            const std::array<int, 3> across = look(slanted);
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(across[channel], throughFlatWater(depth, slanted, channel), 1)
                    << "channel " << channel << " under a slanted sun";

            // The parameter has to matter, and it does in the direction physics says: a slanted sun
            // both meets the bed at a cosine and crosses more water to get there, so less of it
            // comes back. Red, which the extra 36 units of water costs the most.
            EXPECT_LT(across[0], overhead[0]) << "a slanted sun reaches the bed with less left";
        }

        /// Deep water settles at what it scatters, and at half what only-the-return-leg would give.
        ///
        /// Light scattered toward the eye had to get down there first. Attenuating only the way back
        /// lets deep water asymptote to the scattering colour at full sky brightness, which is a
        /// milky sheet rather than a channel. Integrating both legs replaces `1 - T` with
        /// `(1 - T^2) / 2` — the same answer in the shallows, half as bright where it settles.
        ///
        /// Two thousand units down, red's transmittance is `exp(-9.144)`, a part in ten thousand, so
        /// what comes back is the scattering term almost alone:
        ///
        ///   0.5 * 1.07e-4 + 0.012 * 0.5 * 1.0 = 0.0060535
        ///   times the 0.98 that is not Fresnel  = 0.0059324
        ///   1.055 * 0.0059324^(1/2.4) - 0.055   = 0.06958, or 18 of 255
        ///
        /// Only the return leg would put the same pixel at 28 — the factor of two, made visible.
        TEST_F(RtxVisibilityTest, deepWaterSettlesAtHalfWhatOneAttenuatedLegWouldGive)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float depth = 2000.0f;

            const SceneDesc scene = makeFlooded(4000.0f, depth);

            // No sun and a black sky, so the ambient is the only light and the two per cent that
            // reflects off the surface reflects nothing.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, 400.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
            camera.mAmbient = osg::Vec3f(1.0f, 1.0f, 1.0f);

            // No height at all, which is a flat sea: a table whose amplitudes are zero.
            std::vector<std::uint8_t> pixels;
            countHits(scene, {}, camera, size, pixels, SeaState{ .mSignificantHeight = 0.0f });

            EXPECT_EQ(pixels[centre], 18) << "red, settled at what the water scatters";
        }

        /// Broken water is where a wave can break *and* could get to, and no one of the depth, the
        /// sea state or the bed decides that on its own.
        ///
        /// **No sun, so the answer is arithmetic rather than a pattern.** Foam is Lambertian and
        /// spectrally flat, so what a covered surface sends back under an ambient of one and nothing
        /// else is `WATER_FOAM_ALBEDO` in every channel alike — and equal channels are a signature
        /// the water under it can never forge: Jerlov's coastal extinction takes red out first, so
        /// any depth of water at all reads green.
        ///
        /// **Four legs, because the criterion has three terms.** A shelving shallow foams; the same
        /// shore under a sea too small to break in it does not; the same sea over deep water does
        /// not; and — the leg this grew by — the same water over a *level* bed does not either,
        /// however shallow it is. McCowan puts the break at `H / 0.78` and the last leg is what says
        /// that a wave has to have arrived as well, which is what tells a shore from a puddle lying
        /// behind one.
        TEST_F(RtxVisibilityTest, surfCoversShallowWaterThatAWaveCanBothBreakInAndReach)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            const auto look = [&](const SceneDesc& scene, float significantHeight) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -1.0f, 400.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mAmbient = osg::Vec3f(1.0f, 1.0f, 1.0f);
                camera.mWaterLevel = 0.0f;

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, SeaState{ .mSignificantHeight = significantHeight });
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // Two units of water on a bed falling away at forty-five degrees, under a sea whose
            // significant height is forty and so breaks out in fifty-one units of depth. The centre
            // is covered whatever the wave over it is doing.
            const std::array<int, 3> surf = look(makeShelving(4000.0f, 2.0f, 1.0f), 40.0f);
            EXPECT_EQ(surf[0], surf[1]) << "a bubble raft has no colour of its own";
            EXPECT_EQ(surf[1], surf[2]) << "and none in the other channel either";

            // **The same shore under a sea that cannot break in it.** A significant height of one
            // breaks in 1.28 units and there are two here, so the criterion is never met and what
            // comes back is the bed through two units of water that barely tint it.
            const std::array<int, 3> calm = look(makeShelving(4000.0f, 2.0f, 1.0f), 1.0f);
            EXPECT_LT(calm[0], surf[0]) << "no foam, and the bed shows through instead";
            EXPECT_EQ(calm[0], calm[1]) << "two units of water take out nothing worth measuring";

            // **And the same sea over water deep enough to hold it.** Nothing about the waves
            // changed; the column under them did. What comes back is water — dark and green,
            // because red went first.
            const std::array<int, 3> deep = look(makeShelving(4000.0f, 400.0f, 1.0f), 40.0f);
            EXPECT_LT(deep[0], surf[0]) << "no foam over deep water, however rough the sea";
            EXPECT_LT(deep[0], deep[1]) << "and water is green: red goes first";
            EXPECT_LT(deep[2], deep[1]) << "with blue between the two";

            // **The same two units, the same sea, and a bed that does not go anywhere.** Every
            // depth-and-sea-state term the first leg met, this one meets too; what it has not got is
            // anywhere for a wave to have come from, because a level bed puts the nearest water deep
            // enough to break in an unbounded distance away. This is the pan behind a shoreline, and
            // before the bed was consulted it rendered as solid surf from edge to edge.
            const std::array<int, 3> pan = look(makeFlooded(4000.0f, 2.0f), 40.0f);
            EXPECT_LT(pan[0], surf[0]) << "no foam on water no wave reaches";
            EXPECT_EQ(pan[0], calm[0])
                << "and what is left is the bed, which the shelving leg has to agree with: these two "
                   "beds lie at different angles and nothing here is lit by anything but an ambient, "
                   "which no angle changes";

            // **And the share is the one the arithmetic names, not merely more than none.** The sea
            // breaks in `40 / 0.78 = 51.28` units and there are two here, so the wave broke 49.28
            // units of depth back — down a bed falling at one, 49.28 units away. Foam is carried
            // `3.5 * sqrt(627.1 * 51.28) = 627.7` units before it has gone, so
            // `exp(-49.28 / 627.7) = 0.9245` of what broke is still here, lying over the same bed
            // the calm leg read.
            //
            // **One byte of slack, and it is named.** The water under the foam is a thirteenth of
            // the answer and the calm leg measured it under a different sea, so it stands in for
            // that thirteenth rather than being it. As the two seas stand the substitution costs
            // nothing at all and this lands on the byte exactly; the slack is for the day one of
            // them moves, not for the arithmetic.
            constexpr float covered = 0.9245f;
            const float mixed = covered * Shaders::WATER_FOAM_ALBEDO
                + (1.0f - covered) * decodeSrgb(static_cast<std::uint8_t>(calm[0]));
            EXPECT_NEAR(surf[0], encodeSrgb(mixed), 1) << "the run-out term's own value, not just its sign";
        }

        /// What each mask names, and what neither of them does.
        ///
        /// **A mask that says stop accumulating is a mask that says keep the noise**, so what it
        /// names has to be only what no motion vector describes. It once named all water as well, on
        /// the reasoning that a reflection moves with the surface carrying it — which was true, and
        /// stopped being the answer the moment water got a reflection vector of its own. A third of
        /// a Balmora frame was being held at one sample for a question that had been answered.
        ///
        /// What is left is the pixel a sprite reached and did not win: some share of what it shows
        /// went somewhere the vector written for it does not point.
        TEST_F(RtxVisibilityTest, theBiasMaskNamesOnlyWhatNoMotionVectorDescribes)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = std::size_t{ size / 2 } * size + size / 2;

            // The ladder's first level is 40 of 255, so a sprite cut from it covers a sixth of what
            // is behind it — it reaches the pixel and comes nowhere near owning it.
            TestTexture ladder;
            makeMipLadder(ladder);
            const std::span<const TextureData> textures(&ladder.mData, 1);

            SceneDesc scene = makeFlooded(4000.0f, 40.0f);
            const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));
            const std::array<Sprite, 1> sprites{ Sprite{
                .mPosition = osg::Vec3f(-80.0f, 0.0f, 200.0f), .mRadius = 40.0f, .mAlpha = 1.0f } };
            scene.addEmitter(sprites, cut, false);

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, 400.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
            camera.mAmbient = osg::Vec3f(1.0f, 1.0f, 1.0f);
            camera.mWaterLevel = 0.0f;

            std::vector<std::uint8_t> pixels;
            countHits(scene, textures, camera, size, pixels, SeaState{});

            std::vector<float> particles;
            std::vector<float> bias;
            mRenderer->readChannel(Channel::ParticleMask, particles);
            mRenderer->readChannel(Channel::BiasMask, bias);
            ASSERT_EQ(particles.size(), std::size_t{ size } * size);

            const auto sprited = std::find(particles.begin(), particles.end(), 1.0f);
            ASSERT_NE(sprited, particles.end()) << "the emitter reached no pixel at all";
            const std::size_t covered = static_cast<std::size_t>(sprited - particles.begin());

            EXPECT_EQ(bias[covered], 1.0f) << "a sixth of a pixel of sprite wins no vector and is described by none";

            // **The centre is water and nothing else**, which is the whole of the fix: it reflects,
            // it has a vector for what it reflects, and it is not on either mask.
            EXPECT_EQ(particles[centre], 0.0f) << "no sprite over the middle of the frame";
            EXPECT_EQ(bias[centre], 0.0f) << "and water is described rather than given up on";

            // The two are still different populations, and both are a small part of the frame.
            const std::ptrdiff_t marked = std::count(bias.begin(), bias.end(), 1.0f);
            EXPECT_GT(marked, 0);
            EXPECT_LT(marked, static_cast<std::ptrdiff_t>(bias.size()) / 4)
                << "a bias mask over a quarter of a frame is a quarter of a frame held at one sample";
        }

        /// A pixel a sprite mostly is moves the way that sprite did, and not the way the wall
        /// behind it did.
        ///
        /// **The half of the problem the masks only apologise for.** One motion vector is written
        /// per pixel, and it used to be the surface's whatever stood in front of it — so a raindrop
        /// crossing a wall was reprojected as though it were the wall, every frame. The particle
        /// carries its own travel now, off `osgParticle`'s own previous position.
        TEST_F(RtxVisibilityTest, aPixelASpriteOwnsCarriesTheSpritesOwnMotion)
        {
            constexpr std::uint32_t size = 33;

            // Two hundred units under the eye, so a unit across is `size / (2 * 200 * tan(30 deg))`
            // of a pixel: 33 / 230.94 = 0.14289. A sprite that travelled sixty units across is
            // 8.573 pixels of screen motion, and nothing else in the frame moves at all.
            constexpr float travel = 60.0f;
            constexpr float expected = 33.0f * travel / (2.0f * 200.0f * 0.5773503f);

            TestTexture sheet;
            makeOpaqueSheet(sheet);
            const std::span<const TextureData> textures(&sheet.mData, 1);

            SceneDesc scene = makeFlooded(4000.0f, 40.0f);
            const Index cut = scene.addTexture(VFS::Path::NormalizedView("sprite.dds"));
            const std::array<Sprite, 1> sprites{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 200.0f),
                .mRadius = 40.0f,
                .mAlpha = 1.0f,
                .mMoved = osg::Vec3f(travel, 0.0f, 0.0f) } };
            scene.addEmitter(sprites, cut, false);

            const Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, 400.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

            // **Twice, with the camera held still.** The first frame has no past to reproject
            // against, so what the second one writes is the sprite's travel and nothing else.
            std::vector<std::uint8_t> pixels;
            countHits(scene, textures, camera, size, pixels, SeaState{});
            mRenderer->renderFrame(camera, FrameOptions{});

            std::vector<float> covered;
            std::vector<float> moved;
            mRenderer->readChannel(Channel::ParticleMask, covered);
            mRenderer->readChannel(Channel::Motion, moved);

            const auto sprited = std::find(covered.begin(), covered.end(), 1.0f);
            ASSERT_NE(sprited, covered.end()) << "the emitter reached no pixel at all";
            const std::size_t at = static_cast<std::size_t>(sprited - covered.begin());

            // A corner, which the sprite is nowhere near: still water under a still camera.
            EXPECT_NEAR(moved[0], 0.0f, 0.01f) << "nothing else in the frame moved";
            EXPECT_NEAR(moved[1], 0.0f, 0.01f);

            EXPECT_NEAR(std::abs(moved[at * 2]), expected, 0.5f)
                << "the sprite's own travel, projected at the depth it hangs at";
            EXPECT_NEAR(moved[at * 2 + 1], 0.0f, 0.5f) << "and it travelled across rather than along";

            // **The parameter has to matter**, or this measures a coincidence: the same frame with a
            // particle that did not move writes the surface's nought instead.
            SceneDesc still = makeFlooded(4000.0f, 40.0f);
            const Index cutAgain = still.addTexture(VFS::Path::NormalizedView("sprite.dds"));
            const std::array<Sprite, 1> stopped{ Sprite{
                .mPosition = osg::Vec3f(0.0f, 0.0f, 200.0f), .mRadius = 40.0f, .mAlpha = 1.0f } };
            still.addEmitter(stopped, cutAgain, false);

            countHits(still, textures, camera, size, pixels, SeaState{});
            mRenderer->renderFrame(camera, FrameOptions{});
            mRenderer->readChannel(Channel::Motion, moved);

            EXPECT_NEAR(moved[at * 2], 0.0f, 0.01f) << "a particle that stood still moved nothing";

            // **And the kind that hides nothing.** A flame blends additively, so it leaves the
            // transmittance at one however bright it is: no measure of coverage will ever find it,
            // and the rule that only asked about coverage left its glow reprojected as the water
            // under it. It owns the pixel by outshining what the layer left instead.
            SceneDesc flame = makeFlooded(4000.0f, 40.0f);
            const Index cutFlame = flame.addTexture(VFS::Path::NormalizedView("sprite.dds"));
            const std::array<Sprite, 1> burning{ Sprite{ .mPosition = osg::Vec3f(0.0f, 0.0f, 200.0f),
                .mRadius = 40.0f,
                .mAlpha = 1.0f,
                .mMoved = osg::Vec3f(travel, 0.0f, 0.0f) } };
            flame.addEmitter(burning, cutFlame, true);

            countHits(flame, textures, camera, size, pixels, SeaState{});
            mRenderer->renderFrame(camera, FrameOptions{});

            std::vector<float> lit;
            mRenderer->readChannel(Channel::ParticleMask, lit);
            mRenderer->readChannel(Channel::Motion, moved);

            const auto glowing = std::find(lit.begin(), lit.end(), 1.0f);
            ASSERT_NE(glowing, lit.end()) << "an additive sprite is still a sprite the mask names";
            const std::size_t over = static_cast<std::size_t>(glowing - lit.begin());

            EXPECT_NEAR(std::abs(moved[over * 2]), expected, 0.5f)
                << "the flame's own travel, though it covered nothing at all";
        }

        /// A reflection is not where the water is, and reprojects from where its image is.
        ///
        /// **The one surface in the frame that shows something standing somewhere else.** Water is
        /// shaded on the primary hit, so the ordinary motion vector describes the water — and a
        /// shoreline mirrored in a lake then swims as the camera walks, because it is being
        /// reprojected at the depth of the surface carrying it rather than at the depth of its own
        /// image.
        TEST_F(RtxVisibilityTest, aReflectionReprojectsFromWhereItsImageIsAndNotFromTheWater)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = std::size_t{ size / 2 } * size + size / 2;

            // A ceiling two hundred units over the water, with the eye a hundred up between them
            // looking straight down. The water under the eye is a hundred away; the ceiling's image
            // in the plane is at `z = -200`, which is three hundred — so a step sideways moves the
            // reflection exactly a third of what it moves the surface, and only a mirrored
            // reprojection can produce that. Reprojecting the ceiling where it actually stands would
            // put it behind the camera.
            constexpr float step = 10.0f;
            constexpr float halfHeight = 0.5773503f;
            constexpr float surface = size * step / (2.0f * 100.0f * halfHeight);
            constexpr float image = size * step / (2.0f * 300.0f * halfHeight);

            SceneDesc scene = makeOpenWater(4000.0f);
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(4000.0f, 200.0f), {}, {}, sQuadIndices) });

            const auto look = [&](float across) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(across, -1.0f, 100.0f), osg::Vec3f(across, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

                // The plane the water geometry lies in, which the mirrored reprojection reflects
                // about. A scene with water in it and no level named is not one this renderer makes.
                camera.mWaterLevel = 0.0f;
                return camera;
            };

            // **A flat sea, because this is a claim about the plane.** The image of a point in a
            // tilted facet is not its image in the plane, so a wave scatters the reflection by
            // exactly the amount this reprojection cannot describe — which is what the bias mask
            // beside it is for, and not what is being measured here.
            constexpr SeaState still{ .mSignificantHeight = 0.0f };

            std::vector<std::uint8_t> pixels;
            countHits(scene, {}, look(0.0f), size, pixels, still);

            std::vector<float> mirrored;
            std::vector<float> moved;
            mRenderer->readChannel(Channel::ReflectionMotion, mirrored);
            mRenderer->readChannel(Channel::Motion, moved);
            ASSERT_EQ(mirrored.size(), std::size_t{ size } * size * 2);

            // **Nothing has moved yet**, which the mirrored field has to say as plainly as the
            // ordinary one: a still camera over still water reflects a still ceiling.
            mRenderer->renderFrame(look(0.0f), FrameOptions{});
            mRenderer->readChannel(Channel::ReflectionMotion, mirrored);
            EXPECT_NEAR(mirrored[centre * 2], 0.0f, 0.01f) << "a still frame reflects a still image";
            EXPECT_NEAR(mirrored[centre * 2 + 1], 0.0f, 0.01f);

            // A step sideways, with everything in the world standing still.
            mRenderer->renderFrame(look(step), FrameOptions{});
            mRenderer->readChannel(Channel::ReflectionMotion, mirrored);
            mRenderer->readChannel(Channel::Motion, moved);

            EXPECT_NEAR(std::abs(moved[centre * 2]), surface, 0.1f) << "the water is a hundred units under the eye";
            EXPECT_NEAR(std::abs(mirrored[centre * 2]), image, 0.1f)
                << "and what it reflects has its image three hundred under, so it moves a third as far";
            EXPECT_LT(std::abs(mirrored[centre * 2]), std::abs(moved[centre * 2]))
                << "which is the whole point: the two are not the same vector";

            // **And the sky reflected is a reflection too.** Take the ceiling away and every one of
            // these rays reaches the sky instead, which has no distance and still moves when the
            // camera turns — writing nought there says the mirrored horizon is nailed to the screen.
            const SceneDesc bare = makeOpenWater(4000.0f);
            const auto turn = [&](float sideways) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -1.0f, 100.0f), osg::Vec3f(sideways, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                camera.mWaterLevel = 0.0f;
                return camera;
            };

            countHits(bare, {}, turn(0.0f), size, pixels, still);
            mRenderer->renderFrame(turn(40.0f), FrameOptions{});
            mRenderer->readChannel(Channel::ReflectionMotion, mirrored);

            EXPECT_GT(std::abs(mirrored[centre * 2]), 1.0f) << "the mirrored sky slid when the camera turned";
            EXPECT_LT(std::abs(mirrored[centre * 2]), static_cast<float>(size)) << "and stayed on screen";
        }

        /// The same column of water has to look the same from either side of it.
        ///
        /// **This is the test that found the missing half.** Seen from ten units above, a ray crosses
        /// the surface, the water and back; seen from ten units below, it crosses the water and the
        /// ray is fogged by it directly. Those are different code paths through the same physics, and
        /// they have to agree — which they cannot while the light reaching the bottom is not
        /// attenuated by the water over it.
        ///
        /// M6's stated done-when, and the sort of thing that is found by measuring rather than by
        /// looking: neither view is obviously wrong on its own.
        TEST_F(RtxVisibilityTest, aColumnOfWaterAgreesFromAboveAndFromBelow)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float depth = 200.0f;

            const SceneDesc scene = makeFlooded(400.0f, depth);

            // A fifth of a degree off the vertical, which `makeCamera` insists on and which changes
            // the path by a part in ten thousand.
            const auto look = [&](float from) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -0.05f, from), osg::Vec3f(0.0f, 0.0f, from - 10.0f), 60.0f, size, size, 10000.0f);
                litThroughWater(camera);

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, SeaState{ .mSignificantHeight = 0.0f });
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // Ten above and ten below, and the two legitimately differ by a little. From above the
            // ray crosses the whole 200 units after the surface and loses two per cent to Fresnel on
            // the way in; from below it crosses 190 and meets no surface at all. In radiance that is
            //
            //   exp(0.004572 * 10) / 0.98 = 1.068
            //
            // for red, which the sRGB curve compresses to about three per cent of a byte — so 63 and
            // 65, and a tolerance of two rather than a round fraction chosen to pass.
            const std::array<int, 3> above = look(10.0f);
            const std::array<int, 3> below = look(-10.0f);

            EXPECT_GT(above[0], 0) << "the bed is visible from above";
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(below[channel], above[channel], 2) << "channel " << channel << ": " << above[channel]
                                                               << " from above, " << below[channel] << " from below";
        }

        /// The sky loses the column of water over a bed just as the sun does.
        ///
        /// **The half a bounce could quietly skip.** A bounce that escapes to the sky is the sky
        /// arriving at the point it left, so it crosses the same depth the sun crosses and has to
        /// lose the same fraction to it. Returning the sky whole lights a submerged floor as though
        /// the water above it were not there — the fault `aColumnOfWaterAgreesFromAboveAndFromBelow`
        /// was written for, in the one term that test cannot see: a bounce shades the same bed
        /// identically from either side of the surface, so the two views agree while both are wrong.
        ///
        /// **From under the water, because only a primary hit bounces.** A bed seen down through the
        /// surface is the far end of `waterRay`, which terminates it with `pathEnd` — the flat
        /// ambient — precisely so that a reflection cannot recurse. Put the eye below and the bed is
        /// what the camera ray found, and it gathers a real hemisphere.
        ///
        /// **A sky of one radiance, which makes that hemisphere exact rather than noisy.** Every
        /// direction returns the same number, so the single sample carries no variance and what
        /// follows is an equality rather than an average.
        ///
        /// Red, off a bed two hundred units down, seen from a hundred and ninety units above it:
        ///
        ///   down  = exp(-0.004572 * 200)         = 0.400757   the sky's own way in
        ///   bed   = 0.5 albedo * 0.6 sky * down  = 0.120227   what leaves it
        ///   out   = exp(-0.004572 * 190)         = 0.419505   and the way back to the eye
        ///   pixel = bed * out                    = 0.050436,  or 63 of 255
        ///
        /// No Fresnel and no surface: the ray never crosses one. Green and blue absorb far less and
        /// land at 131 and 121. Drop the column and red reads 99 — half the scale away, so this
        /// cannot be passed by widening a tolerance.
        TEST_F(RtxVisibilityTest, aBounceIntoTheSkyLosesTheWaterOverTheBedItLeft)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float depth = 200.0f;
            constexpr float above = 190.0f;
            constexpr float sky = 0.6f;

            const SceneDesc scene = makeFlooded(4000.0f, depth);

            // Straight down but for the third of a degree `makeCamera` insists on — it refuses a
            // view along the world's up axis, where roll has no answer — so the way out is the
            // vertical column to a part in seventy thousand and the bed is met square on.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, above - depth), osg::Vec3f(0.0f, 0.0f, -depth), 60.0f, size, size, 10000.0f);
            camera.mSkyHorizon = osg::Vec3f(sky, sky, sky);
            camera.mSkyZenith = camera.mSkyHorizon;
            camera.mWaterLevel = 0.0f;

            // Flat, so the surface the bounce passes through neither bends it nor gathers it.
            std::vector<std::uint8_t> pixels;
            countHits(scene, {}, camera, size, pixels, SeaState{ .mSignificantHeight = 0.0f });

            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const float down = std::exp(-Shaders::WATER_EXTINCTION[channel] * depth);
                const float out = std::exp(-Shaders::WATER_EXTINCTION[channel] * above);

                EXPECT_NEAR(pixels[centre + channel], int{ encodeSrgb(0.5f * sky * down * out) }, 1)
                    << "channel " << channel;
            }
        }

        /// The waves gather the sun into moving lines on the bed, and move light rather than make it.
        ///
        /// Caustics are ray density — the determinant of the Jacobian of the map from where light
        /// met the surface to where it landed — and what isolates that term from everything else in
        /// the frame is a **ratio**: the same bed, camera and sun, rendered with a sea state and
        /// without one. Dividing the two cancels the albedo, the absorption, the cosine and the
        /// geometry, and leaves the term itself, pixel for pixel.
        ///
        /// **Looked at from underneath**, so no wavy surface stands between the eye and the bed.
        /// Seen from above, what the waves do to the *view* would be mixed into what they do to the
        /// *light*, and the ratio would measure both.
        TEST_F(RtxVisibilityTest, theWavesGatherSunlightOntoTheBedWithoutMakingAnyOfIt)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::size_t count = std::size_t{ size } * size;

            // A wide look from 135 units above the bed *whatever the depth is*, so that two depths
            // see the same patch of water and their caustics can be compared pixel for pixel.
            const auto render = [&](float depth, const SeaState& sea, float seconds) {
                const SceneDesc scene = makeFlooded(4000.0f, depth);

                Shaders::VisibilityConstants camera = makeCamera(osg::Vec3f(0.0f, -1.0f, 135.0f - depth),
                    osg::Vec3f(0.0f, 0.0f, -depth), 90.0f, size, size, 100000.0f);
                litThroughWater(camera);
                camera.mTime = seconds;

                std::vector<std::uint8_t> image;
                countHits(scene, {}, camera, size, image, sea);
                return image;
            };

            // Green, which survives the water best of the three and so has the most of a byte left
            // to vary over.
            const auto causticField = [&](float depth, float seconds = 0.0f) {
                // The still sea does not move, so one baseline serves whatever the clock says.
                const std::vector<std::uint8_t> still = render(depth, SeaState{ .mSignificantHeight = 0.0f }, 0.0f);
                const std::vector<std::uint8_t> running = render(depth, SeaState{}, seconds);

                std::vector<float> field;
                field.reserve(count);
                for (std::size_t i = 0; i < count; ++i)
                    field.push_back(decodeSrgb(running[i * 4 + 1]) / decodeSrgb(still[i * 4 + 1]));

                return field;
            };

            // A hundred and forty units down, which is where the lens stops sharpening.
            const std::vector<float> capped = causticField(140.0f);
            const auto [dimmest, brightest] = std::minmax_element(capped.begin(), capped.end());

            float total = 0.0f;
            for (const float gathered : capped)
                total += gathered;
            const float mean = total / static_cast<float>(count);

            // Measured over this patch: the brightest place on the bed is gathered to 2.75 of what
            // a flat sea would put there and the dimmest thinned to 0.38, so the pattern is bold
            // rather than a wobble.
            EXPECT_GT(*brightest, 2.0f) << "measured 2.75, gathered into lines";
            EXPECT_LT(*dimmest, 0.6f) << "measured 0.38, and thinned between them";

            // **And the mean is one**, which is the claim that makes it light and not decoration.
            // It comes out at 1.024: a reciprocal of something that fluctuates is worth more than
            // the reciprocal of its mean, and what the shader takes out analytically is the second
            // order of that. **With the correction removed this reads 1.123** — four times the
            // slack allowed here, which is what makes this tolerance a test rather than a comment.
            EXPECT_NEAR(mean, 1.0f, 0.04f) << "the waves redistribute the sun, they do not make any";

            // Past the cap the lens is still the one at a hundred and forty, so a bed four hundred
            // units down gets the *same* pattern rather than a sharper one. Both are looked at from
            // the same height above the bed, so the two frames cover the same water at the same cone
            // width and the fields subtract.
            //
            // What is left is the eight-bit round trip. The deeper bed is darker — green comes back
            // at 119 of 255 against 141 — so half a byte on each of the two reads is about 1.4 per
            // cent of a ratio, and on a pixel gathered to 2.7 that is 0.04. The worst over the frame
            // measures 0.050, and the tolerance is twice it.
            //
            // Without the cap the reference renderer measured three quarters more light than fell
            // at this depth — the fold the small-angle map cannot describe.
            const std::vector<float> deeper = causticField(400.0f);
            for (std::size_t i = 0; i < count; ++i)
                ASSERT_NEAR(deeper[i], capped[i], 0.1f) << "pixel " << i << ", past the depth cap";

            // **How bold the pattern is, and how fast it moves** — M6 asks for both measured rather
            // than eyeballed, and they are the two halves of one choice. The spectrum's short cutoff
            // is a limit in *time*, not in space: the waves that focus hardest are the shortest, and
            // a wave's period falls with its length, so the same waves that make the boldest
            // caustics are the ones that make them tear.
            float spread = 0.0f;
            for (const float gathered : capped)
                spread += (gathered - mean) * (gathered - mean);

            EXPECT_NEAR(std::sqrt(spread / static_cast<float>(count)) / mean, 0.366f, 0.02f)
                << "the pattern's contrast, as a fraction of its own mean";

            // A twelfth of a second, which is how long a frame is worth caring about. For two
            // samples of one field, `E[(b - a)^2] = 2 sigma^2 (1 - rho)`, so half the ratio of the
            // two sums is the share of the pattern that is new — 51.1% here.
            //
            // That is the number the shortest wave was chosen on. The reference renderer's sweep:
            // **18 units gives the best caustics it ever drew and they tear at 73%**, 32 units comes
            // out at 51%, and 50 units is dull at 33%. This fork cuts at 32 and lands on 51 — the
            // same spectrum reproducing the same behaviour, which is worth an assertion because
            // moving the cutoff would silently move this.
            const std::vector<float> later = causticField(140.0f, 1.0f / 12.0f);
            float moved = 0.0f;
            for (std::size_t i = 0; i < count; ++i)
                moved += (later[i] - capped[i]) * (later[i] - capped[i]);

            EXPECT_NEAR(0.5f * moved / spread, 0.511f, 0.03f) << "how much of the pattern is new a twelfth later";
        }

        /// The sun's disc carries exactly its irradiance, however wide the pixel that finds it.
        ///
        /// **The sun is drawn in the sky, not answered by a highlight on each surface that could
        /// reflect it** — so the one thing that must hold is that widening the disc never changes
        /// how much light is in it. A cone twice as wide covers four times the solid angle and has
        /// to be four times dimmer, and that is what makes a rough sea spread the sun without
        /// brightening the sea.
        ///
        /// Two fields of view over the same sun, so the pixel doing the finding is ten times wider
        /// in one than the other. The irradiance is small because nothing here has an exposure stage
        /// and the real thing saturates on sight.
        TEST_F(RtxVisibilityTest, theSunsDiscCarriesItsIrradianceHoweverWideThePixelThatFindsIt)
        {
            constexpr std::uint32_t size = 64;
            constexpr float irradiance = 4.0e-5f;

            struct Disc
            {
                float mPeak;
                float mTotal;
                float mSpreadAngle;
            };

            // Looking straight at a sun 45 degrees up, from clear of the only thing in the scene.
            const auto lookAtTheSun = [&](float fov) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -500.0f, 0.0f), osg::Vec3f(0.0f, -501.0f, 1.0f), fov, size, size, 10000.0f);
                camera.mSunPosition = sunStandingAt(osg::DegreesToRadians(45.0f));
                camera.mSunIrradiance = osg::Vec3f(irradiance, irradiance, irradiance);

                // **The disc is drawn because there is light, which is one fact and not two.** What
                // it is painted with is still its own colour, and white is the plain noon of it.
                camera.mSunDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f);

                const SceneDesc scene = makeWall();
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);

                // A pixel's solid angle is its side squared at these angles: the frame is two
                // degrees across in one case and twenty in the other, where the cos-cubed the exact
                // form carries is a part in two thousand.
                Disc disc{ .mPeak = 0.0f, .mTotal = 0.0f, .mSpreadAngle = camera.mCamera.mSpreadAngle };
                for (std::size_t i = 0; i < std::size_t{ size } * size; ++i)
                {
                    const float radiance = decodeSrgb(pixels[i * 4]);
                    disc.mPeak = std::max(disc.mPeak, radiance);
                    disc.mTotal += radiance * camera.mCamera.mSpreadAngle * camera.mCamera.mSpreadAngle;
                }
                return disc;
            };

            // The same arithmetic the shader does, from the same two constants: a cap of angular
            // radius `r` subtends `pi * (2 sin(r / 2))^2`, and the disc's radiance is the sun's
            // irradiance spread over it.
            const auto capRadiance = [&](float spreadAngle) {
                const float edge = 2.0f * std::sin(0.5f * (Shaders::SUN_ANGULAR_RADIUS + 0.5f * spreadAngle));
                return irradiance / (0.5f * Shaders::TAU * edge * edge);
            };

            // A byte is worth about 0.006 of a linear value up here, so that is the tolerance.
            const Disc fine = lookAtTheSun(2.0f);
            EXPECT_NEAR(fine.mPeak, capRadiance(fine.mSpreadAngle), 0.007f)
                << "a pixel a third of the sun's width across";

            // **And the light in it is the sun's**, integrated over the frame rather than argued
            // for. Two hundred and fifty-six pixels land inside this disc, so the sum is a real
            // quadrature: it comes out at 4.015e-5 against the 4.0e-5 that was put in.
            EXPECT_NEAR(fine.mTotal, irradiance, 0.03f * irradiance) << "the disc holds what the sun sent";

            // Ten times the field of view, so ten times the pixel and a disc widened by 1.6 — which
            // by the cap's solid angle is 2.28 times dimmer, and measures so. **This is the whole
            // mechanism**: the same flux over a larger cap.
            const Disc coarse = lookAtTheSun(20.0f);
            EXPECT_NEAR(coarse.mPeak, capRadiance(coarse.mSpreadAngle), 0.007f)
                << "a pixel wider than the sun, which averages it rather than sampling it";

            // Its total is not asserted, and the reason is the honest one: this disc covers 5.7
            // pixels and four pixel centres fall inside it, so a sum over pixels is quadrature with
            // four samples and lands 30% low. The peak above is the exact statement.
            EXPECT_LT(coarse.mPeak, fine.mPeak) << "a wider cone is a dimmer sun, never a brighter one";
        }

        /// A rough sea spreads the sun into a road across the water, and a flat one does not.
        ///
        /// **This is what the lost slopes are for.** Water too fine for the ray cone is averaged
        /// into a flat facet, and a facet that lost its slope is a mirror: it reflects the sun as
        /// one hard dot, and as the camera moves that dot jumps between pixels — the field of
        /// crawling white sparks that distant water becomes. Handing the variance of what was
        /// dropped to the sun's disc instead is what turns it into a road.
        ///
        /// Far enough out that the cone cannot resolve the waves, which is where the term decides
        /// the picture. Water and sky only, so every byte in the frame is the sun off the surface:
        /// the refraction ray leaves through the bottom, finds nothing, and comes back black.
        TEST_F(RtxVisibilityTest, aRoughSeaSpreadsTheSunIntoARoadAndAFlatOneShowsOneDot)
        {
            constexpr std::uint32_t size = 64;
            constexpr float irradiance = 1.0e-3f;

            struct Road
            {
                int mPeak;
                std::size_t mLit;
            };

            const auto road = [&](const SeaState& sea) {
                const osg::Vec3f eye(0.0f, 0.0f, 6000.0f);
                const osg::Vec3f at(0.0f, 14000.0f, 0.0f);

                Shaders::VisibilityConstants camera = makeCamera(eye, at, 20.0f, size, size, 1000000.0f);

                // The sun put exactly where the centre pixel's reflection points, which is the view
                // mirrored in the water's plane and then reversed into a direction of travel. Off
                // the top of the frame by twice the camera's own tilt, so the disc in the sky and
                // the disc in the water are never in the same picture.
                osg::Vec3f view = at - eye;
                view.normalize();
                camera.mSunPosition = osg::Vec3f(view.x(), view.y(), -view.z());
                camera.mSunIrradiance = osg::Vec3f(irradiance, irradiance, irradiance);
                camera.mSunDiscColour = osg::Vec3f(1.0f, 1.0f, 1.0f);
                camera.mWaterLevel = 0.0f;

                const SceneDesc scene = makeOpenWater(20000.0f);
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, sea);

                Road found{ .mPeak = 0, .mLit = 0 };
                for (std::size_t i = 0; i < std::size_t{ size } * size; ++i)
                {
                    found.mPeak = std::max(found.mPeak, int{ pixels[i * 4] });
                    found.mLit += pixels[i * 4] > 0 ? 1 : 0;
                }
                return found;
            };

            // A flat sea is a mirror and shows the sun once: four pixels of 4,096, at 202 of 255.
            const Road still = road(SeaState{ .mSignificantHeight = 0.0f });
            EXPECT_LT(still.mLit, 20u) << "measured 4 pixels: a mirror shows one dot";
            EXPECT_GT(still.mPeak, 150) << "measured 202: and shows it at full strength";

            // The same sun over a sea with a state in it reaches 1,914 pixels — near half the frame
            // — at a peak of 10. **Nearly five hundred times the area at a twentieth the strength**,
            // which is a road rather than a spark.
            const Road running = road(SeaState{});
            EXPECT_GT(running.mLit, 1000u) << "measured 1914 pixels: the sun spread across the water";
            EXPECT_LT(running.mPeak, 40) << "measured 10: and no pixel of it near the mirror's";
        }

        /// Water too fine for the cone to resolve widens the cone that refracts through it.
        ///
        /// **What the cone could not resolve is not gone, it is rough** — and that roughness has to
        /// reach the texture filter, not only the specular lobe. A seabed seen through a mile of
        /// ruffled water is blurred by the slopes that were averaged away, and read at its sharpest
        /// mip instead it comes back as crawling detail no filter downstream can take out.
        ///
        /// **The sea here is built so that every wave is past the cone's reach.** The spectrum runs
        /// from 32 units up to its peak, so with a peak of 64 a footprint of 66 resolves none of it:
        /// every slope is averaged out and the surface is geometrically flat — the normal is exactly
        /// up, the refraction goes straight down, and nothing wobbles — while the whole spectrum's
        /// variance is still carried as roughness. That leaves the cone's width as the only thing
        /// that differs from a still sea, and a ladder of mips on the bed to read it off.
        TEST_F(RtxVisibilityTest, waterTooFineToResolveWidensTheConeItRefractsThrough)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float height = 12000.0f;
            constexpr float depth = 1500.0f;

            TestTexture ladder;
            makeMipLadder(ladder);
            const std::span<const TextureData> textures(&ladder.mData, 1);

            // A fifth of a degree off the vertical, which is the least `makeCamera` will take.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -50.0f, height), osg::Vec3f(0.0f, 0.0f, -depth), 20.0f, size, size, 100000.0f);
            camera.mWaterLevel = 0.0f;

            // Water over a bed that glows its own ladder. Emissive rather than lit, so the byte is
            // the texture and nothing else: an unlit albedo would need a light, and a light would
            // put its own falloff between the mip and the measurement.
            const auto level = [&](const SeaState& sea) {
                SceneDesc scene = makeOpenWater(4000.0f);

                const Index bed = scene.addMesh(makeSheet(500.0f, -depth), {}, sQuadUv, sQuadIndices);
                const Index glow = scene.addMaterial(
                    Material{ .mEmissive = scene.addTexture(VFS::Path::NormalizedView("ladder.dds")) });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = bed, .mMaterial = glow });

                std::vector<std::uint8_t> pixels;
                countHits(scene, textures, camera, size, pixels, sea);

                // Back out everything between the texture and the byte: the emissive scale, the
                // water the glow crossed on its way up, and the two per cent the surface reflected.
                const float carried = Shaders::EMISSIVE_INTENSITY * std::exp(-Shaders::WATER_EXTINCTION[1] * depth)
                    * (1.0f - Shaders::WATER_F0);
                return ladderLevel(decodeSrgb(pixels[centre + 1]) / carried);
            };

            // How wide the refraction's cone is where it lands, in world units: the pixel's own
            // footprint where it met the water, plus what it gained over the leg down. **Twice the
            // lobe**, because the lobe is an rms angle from the axis and a cone spread is a width —
            // and a quarter of it to begin with, because refraction bends by `1 - 1 / n` of what
            // reflection does, so what is seen *through* a rough surface is blurred that much less.
            const auto coneAtBed = [&](float lobe) {
                const float bent = lobe * (1.0f - 1.0f / Shaders::WATER_IOR);
                return camera.mCamera.mSpreadAngle * height + (camera.mCamera.mSpreadAngle + 2.0f * bent) * depth;
            };

            const SeaState fine{ .mSignificantHeight = 3.0f, .mPeakWavelength = 64.0f };
            const float still = level(SeaState{ .mSignificantHeight = 0.0f });
            const float ruffled = level(fine);

            // The ladder has seven levels and the readout is only meaningful off both ends of it.
            EXPECT_GT(still, 1.0f) << "measured 2.26, clear of the sharpest mip";
            EXPECT_LT(ruffled, 5.0f) << "measured 3.71, clear of the coarsest";

            // Mip level is the log of the cone's width, so the difference is the log of the ratio —
            // and the base term, which is the triangle's texels against its world area, cancels.
            // Predicted 1.474 from the wave table the shader was handed; measured 1.449, which is
            // three quarters of a byte on a ladder 30 bytes to the level.
            //
            // **The factor of two is what this pins.** Adding the lobe once instead of twice puts
            // the prediction at 0.918, sixteen bytes from what the frame actually reads.
            EXPECT_NEAR(ruffled - still, std::log2(coneAtBed(lobeOf(fine)) / coneAtBed(0.0f)), 0.08f)
                << "the cone widened by twice the rms slope the sea could not show";
        }

        /// Fog takes what Beer-Lambert says over the path and gives its own colour back in place.
        ///
        /// **A horizontal ray is what makes the march exact.** The layer's density varies only with
        /// height, so along a ray that holds its own the medium is uniform — and then the march
        /// telescopes: the per-step transmittances multiply to `exp(-sigma * span)` whatever the
        /// step distribution is, and the scattered terms sum to `colour * (1 - T)`. So this asserts
        /// the analytic answer rather than the march's approximation of it.
        TEST_F(RtxVisibilityTest, fogTakesWhatBeerLambertSaysOverThePathAndGivesItsOwnColourBack)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float distance = 2000.0f;
            constexpr float extinction = 3.5e-4f;

            const auto look = [&](float thickness) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -distance, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                litThroughFog(camera, thickness);

                const SceneDesc scene = makeWall();
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // The wall is untextured, so its albedo is 0.5 and the cell's ambient is all that is on
            // it: 0.5 * 0.6 = 0.3. Over that sits `exp(-3.5e-4 * 2000)` = 0.4966 of transmittance,
            // with the rest of the path's worth of the fog's own colour in front of it.
            const float transmittance = std::exp(-extinction * distance);
            const std::array<int, 3> fogged = look(extinction);
            constexpr float wall = 0.5f * sFoggySky;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto expected
                    = static_cast<int>(encodeSrgb(wall * transmittance + sHaze[channel] * (1.0f - transmittance)));

                EXPECT_NEAR(fogged[channel], expected, 1) << "channel " << channel;
            }

            // The three channels also have to come apart the way the fog's colour does, or a grey
            // haze would pass the arithmetic above by accident.
            EXPECT_LT(fogged[0], fogged[1]) << "the fog's own colour, showing through";
            EXPECT_LT(fogged[1], fogged[2]);

            // And no fog is *exactly* no fog. A lit surface with air over it is a differently lit
            // one, which is why every test here that measures radiance leaves this at zero.
            const std::array<int, 3> clear = look(0.0f);
            for (const int level : clear)
                EXPECT_EQ(level, int{ encodeSrgb(wall) }) << "with no fog in the cell";
        }

        /// The fog layer sits on the water, thins with height above it, and stops at the surface.
        ///
        /// Fog gathers over water and drains off high ground, so the level a cell records is where
        /// its layer sits — and *at* it rather than in it, because a point under the surface already
        /// has the water's own absorption over it. Fog there would be a second medium laid on the
        /// first, putting grey between the eye and the seabed twice over.
        TEST_F(RtxVisibilityTest, theFogLayerSitsOnTheWaterThinsAboveItAndStopsAtIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float distance = 2000.0f;
            constexpr float extinction = 3.5e-4f;

            // The ray runs level at z = 0, so how much fog it crosses is decided by where the layer
            // is put under it and by nothing else.
            const auto look = [&](float level, float thickness) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -distance, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                litThroughFog(camera, thickness, level);

                const SceneDesc scene = makeWall();
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return std::array<int, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // A dry cell is handed minus infinity, and falls back to sea level — which is where a
            // water level of zero puts the layer anyway, so the two have to agree exactly.
            const std::array<int, 3> dry = look(-std::numeric_limits<float>::infinity(), extinction);
            const std::array<int, 3> atSeaLevel = look(0.0f, extinction);
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_EQ(dry[channel], atSeaLevel[channel]) << "channel " << channel << ", the dry-cell fallback";

            // Three thousand units under the ray, so it runs `exp(-3000 / 2600)` = 0.3154 of the way
            // up the layer's own falloff and crosses less than a third of the fog.
            constexpr float below = 3000.0f;
            const float thinned = extinction * std::exp(-below / Shaders::FOG_HEIGHT);
            const float transmittance = std::exp(-thinned * distance);

            const std::array<int, 3> high = look(-below, extinction);
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto expected = static_cast<int>(
                    encodeSrgb(0.5f * sFoggySky * transmittance + sHaze[channel] * (1.0f - transmittance)));

                EXPECT_NEAR(high[channel], expected, 1) << "channel " << channel << ", high over the layer";
            }

            // And with the surface over the ray instead there is no air to fog at all: the frame is
            // whatever the water did to it and nothing else, whatever the cell's fog says.
            const std::array<int, 3> submerged = look(100.0f, extinction);
            const std::array<int, 3> withoutFog = look(100.0f, 0.0f);
            for (std::size_t channel = 0; channel < 3; ++channel)
                EXPECT_EQ(submerged[channel], withoutFog[channel]) << "channel " << channel << ", under the surface";
        }

        /// A lamp lights the air it stands in, and by the isotropic share of what it delivers.
        ///
        /// **`INV_FOUR_PI` is what this is really about.** A lamp reaches a point in the fog as
        /// irradiance, exactly as it reaches a surface, and what comes back toward the eye is that
        /// irradiance spread over the whole sphere. Dropping the factor is not subtle — it lights
        /// the air 4pi times over, and the centre pixel here goes from 121 of 255 to 211.
        ///
        /// **The lamp is put far enough away that its falloff is flat along the ray**, which is what
        /// makes the march's answer analytic: with the light it delivers constant, the scattered
        /// terms telescope to `E / 4pi * (1 - T)` exactly as a constant fog colour does. Twenty
        /// thousand units off a two-hundred-unit ray varies the irradiance by 0.02%.
        TEST_F(RtxVisibilityTest, aLampLightsTheAirItStandsInByTheIsotropicShareOfWhatItDelivers)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float distance = 200.0f;
            constexpr float reach = 30000.0f;

            // Half the ray's worth of fog, so the wall and the air contribute comparably.
            const float extinction = std::log(2.0f) / distance;

            // Behind the wall in y, so its cosine there is negative and the lamp lights the air
            // without also lighting what the air is in front of.
            const osg::Vec3f lamp(0.0f, 100.0f, 20000.0f);

            // What one unit of intensity delivers at the middle of the ray, from the same windowed
            // inverse square the shader uses: an inverse square that reaches exactly zero at the
            // light's reach, because Morrowind's is a hard cutoff and clipping one leaves a ring.
            const osg::Vec3f middle(0.0f, -0.5f * distance, 0.0f);
            const float span = (lamp - middle).length();
            const float ratio = span / reach;
            const float window = 1.0f - ratio * ratio * ratio * ratio;
            const float delivered = window * window / (span * span + 1.0f);

            const auto look = [&](bool lit) {
                SceneDesc scene = makeWall();
                if (lit)
                    scene.addLight(Light{
                        .mPosition = lamp,
                        // Scaled so the lamp delivers exactly one unit of irradiance to the ray,
                        // which is what lets the expectation below be written without it.
                        .mIntensity = osg::Vec3f(1.0f, 1.0f, 1.0f) / delivered,
                        .mReach = reach,
                    });

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -distance, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
                litThroughFog(camera, extinction);

                // Black air, so the lamp is the only thing in the frame the fog scatters and the
                // expectation below needs no term for the haze `litThroughFog` would otherwise put
                // in it.
                camera.mFogColour = osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return int{ pixels[centre] };
            };

            // With no lamp the air scatters nothing, because the fog's own colour is black here: the
            // wall is all there is, at half of it.
            constexpr float wall = 0.5f * sFoggySky;
            EXPECT_EQ(look(false), int{ encodeSrgb(0.5f * wall) }) << "black air over a lit wall";

            // And with it, one unit of irradiance through an isotropic sphere, over the half of the
            // ray's light the fog took: 0.15 + 1 / (4 pi) * 0.5 = 0.18979, which encodes to 121.
            const float scattered = 0.25f * Shaders::INV_PI * 0.5f;
            EXPECT_NEAR(look(true), int{ encodeSrgb(0.5f * wall + scattered) }, 1) << "the lamp in the air";
        }

        /// The banked field holds as much air as an even one, which is what `FOG_COVERAGE` is for.
        ///
        /// **The noise redistributes the fog, it does not remove it.** The extinction the host
        /// derived is Morrowind's own view distance turned into a coefficient, and a band that
        /// leaves 40% of the volume clear would silently make the world twice as clear as the game
        /// says. Dividing the coverage by its own mean is what holds the average where it was — and
        /// this is what makes that constant a measurement rather than a note, since moving the band
        /// without re-measuring moves this ratio by the constant's own error.
        ///
        /// **It settles just under one, and that is Jensen's inequality rather than a mistake.** A
        /// banked field's optical depth varies far more than an even one's, and `exp` is convex, so
        /// more light survives the same *average* density. Measured at 0.971 against a thickness of
        /// 0.09 — and the deficit scales with it, so a thinner fog would sit closer to one.
        ///
        /// Nine viewpoints, because one is not a sample: the steps bunch near the camera, so a
        /// single frame weighs one small volume of the field heavily and lands anywhere within six
        /// per cent. Nine brings that to about two.
        TEST_F(RtxVisibilityTest, theBankedFieldHoldsAsMuchAirAsAnEvenOne)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::size_t count = std::size_t{ size } * size;

            const auto air = [&](float uniform, float where) {
                Shaders::VisibilityConstants camera = makeCamera(osg::Vec3f(where, -50000.0f, 0.0f),
                    osg::Vec3f(where, -60000.0f, 0.0f), 90.0f, size, size, 100000.0f);
                camera.mFogColour = osg::Vec3f(1.0f, 1.0f, 1.0f);
                camera.mFogExtinction = 3.0e-6f;
                camera.mFogUniform = uniform;

                // A wall behind the camera, because a scene has to hold something. Every ray runs
                // to `FOG_REACH` and comes back with air and the sky, which is what makes the
                // frame's mean a measurement of the air alone.
                const SceneDesc scene = makeWall();
                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);

                double total = 0.0;
                for (std::size_t i = 0; i < count; ++i)
                    total += double{ decodeSrgb(pixels[i * 4]) };

                return total / static_cast<double>(count);
            };

            double ratio = 0.0;
            constexpr std::array places{ 0.0f, 12345.0f, -31000.0f, 77000.0f, 250000.0f, -140000.0f, 33000.0f,
                -420000.0f, 610000.0f };
            for (const float where : places)
                ratio += air(0.0f, where) / air(1.0f, where);

            ratio /= static_cast<double>(places.size());

            EXPECT_NEAR(ratio, 0.971, 0.05) << "banked air against even air, over nine viewpoints";
        }

        /// The fog scatters the sun forward far harder than back, which is what a Mie phase is for.
        ///
        /// **A single Henyey-Greenstein lobe cannot do this shape.** Real droplets throw a peak
        /// within a degree of the light that is orders of magnitude above anything one `g` reaches,
        /// and still send a sixth of isotropic *backwards* — the blaze around a low sun, and fog not
        /// going black when you turn away from it. Before this the fog was lit by the sky and the
        /// lamps and not at all by the sun, so facing it rendered identically to facing away.
        ///
        /// **A ratio, because it is the only thing the fixture computes exactly.** Two frames differ
        /// in nothing but which side of the camera the sun is on, at the same elevation — so the
        /// column of fog it crosses, the extinction, the transmittance and the march all cancel, and
        /// what is left is `p(26.6 degrees) / p(153.4)`. An isotropic fog would give exactly one.
        TEST_F(RtxVisibilityTest, theFogScattersTheSunForwardFarHarderThanBack)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            // Bright enough to read against eight bits after the fog's own column has taken 83% of
            // it, and the forward case still has to stay inside one.
            constexpr float irradiance = 12.7f;

            // Level, so the centre ray holds one height and the medium along it is uniform — which
            // is what lets the two frames cancel to the phase function alone.
            const auto lookPast = [&](float towardsY) {
                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1000.0f, 0.0f), 60.0f, size, size, 100000.0f);

                // The sun a quarter of the way up, ahead of the camera or behind it. Both carry the
                // same climb, so `fogSunDepth` is the same for each and cancels.
                osg::Vec3f towards(0.0f, towardsY, 0.5f);
                towards.normalize();
                camera.mSunPosition = towards;
                camera.mSunIrradiance = osg::Vec3f(irradiance, irradiance, irradiance);
                camera.mFogExtinction = 3.0e-4f;
                camera.mFogUniform = 1.0f;

                // Nothing to hit and nothing else to scatter: every ray runs out to `FOG_REACH`
                // through even air whose own colour is black, so the pixel is the sun and no more.
                //
                // **Out of reach of the shadow rays and not merely out of the frame.** The march
                // sends one at the sun from every stretch of the ray, and a wall standing at y = 0
                // is squarely in the path of the ones the *backward* case sends — so it shadowed the
                // near steps, at a march offset that decides which, and the ratio below moved by a
                // sixth with the dither. A sheet below the world is past `mFar` in every direction
                // any ray here travels.
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(makeSheet(4000.0f, -200000.0f), {}, {}, sQuadIndices) });

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);

                return decodeSrgb(pixels[centre]);
            };

            const float ahead = lookPast(1.0f);
            const float behind = lookPast(-1.0f);

            // Hand-computed from Jendersie and d'Eon's fit at a droplet diameter of eight microns:
            // the blend is 47.4% Draine over a Henyey-Greenstein peak of g = 0.98447, and at
            // `cos = +-0.8944` that comes to 0.225158 forward against 0.011708 back.
            constexpr float forward = 0.225158f;
            constexpr float backward = 0.011708f;

            EXPECT_NEAR(ahead / behind, forward / backward, 1.0f) << "forward against backward scattering";

            // **And the absolute value, because a ratio cannot see a factor of `4 pi`.** That is the
            // mistake this shape of function invites: normalise the phase so isotropic reads one —
            // the convention a lamp is written in — and both frames grow together while their ratio
            // stays put. What the eye gets is the sun's irradiance times the phase *per steradian*,
            // less the fog's own column on the way down, over a ray that runs to `FOG_REACH`:
            //
            //   12.7 * 0.225158 * exp(-2600 * 3e-4 / 0.44721) * (1 - exp(-3e-4 * 30000)) = 0.4998
            //
            // which the sRGB curve puts at 188 of 255. Four pi times that is white.
            const float climb = 0.5f / std::sqrt(1.25f);
            const float column = std::exp(-Shaders::FOG_HEIGHT * 3.0e-4f / climb);
            const float crossed = 1.0f - std::exp(-3.0e-4f * Shaders::FOG_REACH);

            EXPECT_NEAR(ahead, irradiance * forward * column * crossed, 0.006f)
                << "the sun's own irradiance through the phase function, per steradian";

            // And the backward half is not nothing, which is the other half of why a single lobe
            // will not do: fog behind you still glows.
            EXPECT_GT(behind, 0.01f) << "a sixth of isotropic still comes back";
        }

        /// A lid over the march takes the sun out of the air beneath it, and takes all of it.
        ///
        /// **Exactly what sunless air scatters, not merely less than open air.** A shaft that leaked
        /// a tenth of the sun would still be darker than the open sky beside it, and an assertion
        /// that only asked for darker would pass while it leaked. The lid spans the whole march and
        /// the camera's own ray never reaches it — it runs level while the lid is five hundred units
        /// overhead — so what changes between the two frames is the shadow ray and nothing else.
        TEST_F(RtxVisibilityTest, aLidOverTheMarchTakesTheSunOutOfTheAirBeneathIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;
            constexpr float irradiance = 4.0f;

            const auto look = [&](bool lidded, bool lit) {
                // The same sheet either way, over the march or under it, so the two frames differ
                // in what the shadow ray finds and in nothing else — not in what is in the scene,
                // nor in how large it is.
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(makeSheet(40000.0f, lidded ? 500.0f : -500.0f), {}, {}, sQuadIndices) });

                Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, 0.0f, 0.0f), osg::Vec3f(0.0f, 1000.0f, 0.0f), 60.0f, size, size, 100000.0f);

                // Ahead of the camera and well up, so the phase function has something to scatter
                // forward and every shadow ray still climbs into the lid.
                osg::Vec3f travelling(0.0f, -0.6f, -0.8f);
                travelling.normalize();
                camera.mSunPosition = -travelling;
                camera.mSunIrradiance = lit ? osg::Vec3f(irradiance, irradiance, irradiance) : osg::Vec3f();

                // Even air with a colour of its own, so the frame is never empty and the two sunless
                // cases have something to agree about.
                camera.mFogColour = osg::Vec3f(0.02f, 0.02f, 0.02f);
                camera.mFogExtinction = 2.0e-4f;
                camera.mFogUniform = 1.0f;

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels);
                return int{ pixels[centre] };
            };

            const int open = look(false, true);
            const int shaded = look(true, true);
            const int sunless = look(true, false);

            EXPECT_EQ(shaded, sunless) << "the lid takes all of the sun, not most of it";
            EXPECT_GT(open, shaded + 20) << "and there was a sun to take";
        }

        /// Its own device, because the validation layers allocate and this test counts allocations.
        class RtxFrameCostTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                std::string reason;
                mHarness = Testing::getUnvalidatedHarness(reason);
                if (mHarness == nullptr)
                    GTEST_SKIP() << reason;
            }

            Testing::Harness* mHarness = nullptr;
        };

        /// A frame that changes nothing must not go to the heap.
        ///
        /// The concern is jitter rather than throughput: at sixty frames a second a single
        /// allocator stall is a dropped frame, and an average hides it. What this forbids on the
        /// frame path is a `std::string` built, an unreserved vector grown, a `std::function`
        /// captured, a `make_unique` reached for, or logging that did not compile out.
        ///
        /// Warmed up first, because the first of anything legitimately allocates: descriptor pools
        /// grow, the driver caches its first call, and a command buffer finds its size.
        TEST_F(RtxFrameCostTest, aSteadyFrameDoesNotTouchTheHeap)
        {
            constexpr std::uint32_t size = 64;
            constexpr int warmUpFrames = 8;
            constexpr int measuredFrames = 32;

            // What a frame is allowed, and why. Zero is the claim; a constant rather than a literal
            // zero is so that a path which must allocate can be admitted deliberately, with the
            // number and the reason written down beside it.
            constexpr std::size_t budgetPerFrame = 0;

            const SceneDesc scene = makeWall();

            Device& device = *mHarness->mDevice;
            CommandPool pool(device);
            std::vector<InstanceRecord> records;
            makeInstanceRecords(scene, records);
            Batch setup(pool);
            const SceneAcceleration acceleration(device, setup, scene, records, {});
            const SceneBuffers buffers(device, setup, scene, records);

            const TextureArray textures(device, setup, 0, {});
            const VisibilityPass pass(device, setup, Testing::getShaderDirectory(), textures.getLayout(), true);
            setup.flush();
            const VisibilityInputs inputs{
                .mScene = acceleration.getTopLevel(),
                .mBuffers = &buffers,
                .mIndexBlocks = acceleration.getIndexBlocks(),
                .mTextures = textures.getSet(),
                .mShading = textures.getShading(),
            };

            const GBuffer channels(device, size, size);
            const CompositePass composite(device, pool, Testing::getShaderDirectory());

            // The format the shader declares for `colour`. A narrower one makes every load and store
            // through it undefined for the whole image, which the layers say and nothing else does.
            Image target(device, size, size, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, "cost-target");
            const Buffer hits(device, sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            // Once, so that the frames below start from laid-out images rather than each paying
            // transitions the real loop pays at startup.
            pool.submitAndWait([&](VkCommandBuffer commands) {
                channels.begin(commands);
                target.transition(commands, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            });

            // A command buffer reused against a fence, which is what a frame is and what the window
            // loop does. `submitAndWait` would be the setup shape — a fresh buffer from the pool and
            // a wait on the whole queue — and it is measured here as allocating nothing either, so
            // this is about what is being pinned rather than about what it costs today.
            const std::vector<VkCommandBuffer> recorded = pool.allocate(1);
            const VkCommandBuffer commands = recorded.front();

            const VkFenceCreateInfo describeFence{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkFence finished = VK_NULL_HANDLE;
            ASSERT_EQ(vkCreateFence(device.getHandle(), &describeFence, nullptr, &finished), VK_SUCCESS);

            // Everything a still frame does — both passes, because a frame is two now: the camera
            // worked out, the trace and the composite recorded, the work submitted and waited on.
            // The camera is the same every time, which is what "steady" means — a moving one would
            // still allocate nothing, but then nothing would be pinned.
            const auto frame = [&] {
                const Shaders::VisibilityConstants camera = makeCamera(
                    osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

                vkResetCommandBuffer(commands, 0);
                const VkCommandBufferBeginInfo begin{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                };
                vkBeginCommandBuffer(commands, &begin);
                channels.begin(commands);
                pass.record(commands, inputs, channels, hits, camera);
                channels.handOver(commands);
                // No history: a still frame averages nothing, and the pass stands in for the
                // binding rather than making the caller carry an image it never reads.
                composite.record(commands, channels, channels.getIndirect(), nullptr, target,
                    Shaders::CompositeConstants{ .mWidth = size, .mHeight = size, .mAccumulate = 0 });
                vkEndCommandBuffer(commands);

                const VkCommandBufferSubmitInfo buffer{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                    .commandBuffer = commands,
                };
                const VkSubmitInfo2 submit{
                    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                    .commandBufferInfoCount = 1,
                    .pCommandBufferInfos = &buffer,
                };
                vkQueueSubmit2(device.getQueue(), 1, &submit, finished);

                // **Its own submit, but not its own wait.** The shape above is the point of the test
                // — a command buffer reused against a fence is what a frame is — so it does not go
                // through `submitAndWait`, which allocates a buffer per call. The wait is another
                // matter: forty of them with no deadline is what turned one stalled submit into a
                // suite that never finished and a process that had to be killed.
                Rtx::awaitVk(device.getHandle(), finished, "a measured frame");
                vkResetFences(device.getHandle(), 1, &finished);
            };

            for (int i = 0; i < warmUpFrames; ++i)
                frame();

            const std::size_t before = Testing::getAllocationCount();
            for (int i = 0; i < measuredFrames; ++i)
                frame();
            const std::size_t after = Testing::getAllocationCount();

            vkDestroyFence(device.getHandle(), finished, nullptr);

            EXPECT_LE(after - before, budgetPerFrame * measuredFrames)
                << (after - before) << " allocations across " << measuredFrames << " frames";
        }

        /// The same scene with the camera turned around. Nothing is in front of it, so nothing is hit
        /// — the check that the pass reports geometry rather than reporting that it ran.
        TEST_F(RtxVisibilityTest, aCameraFacingAwayHitsNothingAndTheImageIsAllSky)
        {
            constexpr std::uint32_t size = 64;
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, -200.0f, 0.0f), 60.0f, size, size, 10000.0f);

            // A sky with green in it and nothing else, so that "this is sky" and "this is the
            // untextured wall" cannot be confused: the wall is grey through every channel.
            camera.mSkyHorizon = osg::Vec3f(0.0f, 0.25f, 0.0f);
            camera.mSkyZenith = osg::Vec3f(0.0f, 0.25f, 0.0f);

            std::vector<std::uint8_t> pixels;
            EXPECT_EQ(countHits(makeWall(), {}, camera, size, pixels), 0u);

            // Flat, so every pixel is the same byte: 1.055 * 0.25^(1/2.4) - 0.055 = 0.537099, which
            // is 137 of 255.
            ASSERT_EQ(pixels.size(), std::size_t{ size } * size * 4);
            for (std::size_t i = 0; i < pixels.size(); i += 4)
            {
                ASSERT_EQ(pixels[i], 0) << "red at pixel " << i / 4;
                ASSERT_EQ(pixels[i + 1], 137) << "green at pixel " << i / 4;
            }
        }

        /// A mesh moved by its instance and a mesh whose vertices were already moved must render to
        /// the same bytes.
        ///
        /// The transform path has a unit test, and a unit test is not enough on its own: every other
        /// test here places its geometry with the identity, so a transposed rotation would sail
        /// through all of them. This is the one that puts a rotation through the acceleration
        /// structure and compares the result against arithmetic done on the CPU.
        TEST_F(RtxVisibilityTest, aRotatedInstanceRendersAsIfItsVerticesHadBeenMoved)
        {
            const osg::Matrixf transform = osg::Matrixf::scale(1.5f, 1.5f, 1.5f)
                * osg::Matrixf::rotate(osg::DegreesToRadians(37.0f), osg::Vec3f(0.3f, -0.5f, 0.8f))
                * osg::Matrixf::translate(11.0f, -23.0f, 5.0f);

            const std::array local{
                osg::Vec3f(-120.0f, 0.0f, -80.0f),
                osg::Vec3f(120.0f, 0.0f, -80.0f),
                osg::Vec3f(90.0f, 0.0f, 110.0f),
                osg::Vec3f(-140.0f, 0.0f, 60.0f),
            };

            SceneDesc placedByInstance;
            placedByInstance.addInstance(MeshInstance{
                .mTransform = transform, .mMesh = placedByInstance.addMesh(local, {}, {}, sQuadIndices) });

            std::array<osg::Vec3f, 4> moved{};
            for (std::size_t i = 0; i < local.size(); ++i)
                moved[i] = local[i] * transform;

            SceneDesc placedByVertex;
            placedByVertex.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = placedByVertex.addMesh(moved, {}, {}, sQuadIndices) });

            constexpr std::uint32_t size = 64;
            const osg::Vec3f centre(11.0f, -23.0f, 5.0f);
            const Shaders::VisibilityConstants camera
                = makeCamera(centre - osg::Vec3f(0.0f, 260.0f, 0.0f), centre, 60.0f, size, size, 10000.0f);

            std::vector<std::uint8_t> byInstance;
            std::vector<std::uint8_t> byVertex;
            const std::uint32_t instanceHits = countHits(placedByInstance, {}, camera, size, byInstance);
            const std::uint32_t vertexHits = countHits(placedByVertex, {}, camera, size, byVertex);

            // Both blank would agree for the wrong reason.
            ASSERT_GT(vertexHits, 0u);
            EXPECT_EQ(instanceHits, vertexHits);
            EXPECT_EQ(byInstance, byVertex);
        }

        /// The denoiser, measured against the estimator it is smoothing.
        ///
        /// One flat floor under an open sky, so every pixel is one bounce off the same normal with
        /// the same answer in expectation: the mean is fixed and the scatter around it is pure
        /// sampling noise. A filter has one job on a surface like this — take the scatter away and
        /// leave the mean where it was — and both halves are asserted, because a filter that dimmed
        /// the picture would pass a test that only looked at the noise.
        TEST_F(RtxVisibilityTest, theFilterTakesTheNoiseOffAFlatSurfaceAndLeavesTheLightWhereItWas)
        {
            constexpr std::uint32_t size = 64;
            constexpr float samples = float{ size } * size;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(4000.0f, 0.0f), {}, {}, sQuadIndices) });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1.0f, 300.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
            camera.mSkyHorizon = osg::Vec3f(0.20f, 0.15f, 0.60f);
            camera.mSkyZenith = osg::Vec3f(0.80f, 0.65f, 0.15f);

            const auto shade = [&](bool filter) {
                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, {}, camera, size, pixels, SeaState{}, 0, filter), size * size);
                return pixels;
            };

            const auto measure = [&](const std::vector<std::uint8_t>& pixels, std::size_t channel) {
                float sum = 0.0f;
                float squares = 0.0f;
                for (std::size_t i = channel; i < pixels.size(); i += 4)
                {
                    const float linear = decodeSrgb(pixels[i]);
                    sum += linear;
                    squares += linear * linear;
                }

                const float mean = sum / samples;
                return std::pair{ mean, std::sqrt(std::max(squares / samples - mean * mean, 0.0f)) };
            };

            const std::vector<std::uint8_t> raw = shade(false);
            const std::vector<std::uint8_t> filtered = shade(true);

            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto [rawMean, rawSpread] = measure(raw, channel);
                const auto [filteredMean, filteredSpread] = measure(filtered, channel);

                // Half an sRGB step at this brightness, which is the most the two can differ by
                // without one of them having moved the light.
                EXPECT_NEAR(filteredMean, rawMean, 0.004f) << "channel " << channel << " keeps its light";

                EXPECT_LT(filteredSpread, rawSpread * 0.2f)
                    << "channel " << channel << " has most of its noise taken away";
            }
        }

        /// The same floor at a grazing angle, against the answer it is trying to reach.
        ///
        /// **Terrain is nearly always seen this way, and it is the case a depth test gets wrong.**
        /// Pixels down a grazing surface stand a long way apart in distance while remaining one
        /// flat plane, so a filter that refused taps by how far away they are keeps only the taps
        /// across the slope and throws away the ones along it — it still smooths, just half as
        /// well, which is why this measures the error rather than the smoothness. Weighing by how
        /// far a tap sits off the centre pixel's tangent plane costs one dot product and asks the
        /// question that was meant.
        ///
        /// The reference is what `--accumulate` builds: sixty-four differently seeded samples of
        /// the same unbiased estimator, averaged. One sample against that is the error a denoiser
        /// exists to reduce, and the ratio of the two is the only honest way to say it worked.
        ///
        /// **The bound sits between the two weightings on purpose.** Measured here on
        /// `Channel::Radiance`: one sample is 0.0420 off the reference and the plane weight brings
        /// that to 0.0020, against 0.0061 for a plain depth weight — twenty times better against
        /// seven. Every number is repeatable, because frame zero and a sixty-four frame average are
        /// both deterministic, so a tenth is a bound this passes with room and a depth test cannot
        /// reach.
        TEST_F(RtxVisibilityTest, theFilterAndItsHistoryConvergeOnAGrazingSurface)
        {
            constexpr std::uint32_t size = 64;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(40000.0f, 0.0f), {}, {}, sQuadIndices) });

            // A degree and a half above the floor: the horizon sits near the top of the frame and
            // the ground runs from a few hundred units away to eight thousand, so the distance
            // between vertical neighbours changes by more than a pixel footprint nearly everywhere.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -8000.0f, 200.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
            camera.mSkyHorizon = osg::Vec3f(0.20f, 0.15f, 0.60f);
            camera.mSkyZenith = osg::Vec3f(0.80f, 0.65f, 0.15f);

            const auto render = [&](std::uint32_t accumulate, bool filter) {
                std::vector<float> values;
                renderRadiance(scene, camera, size, values, accumulate, filter);
                return values;
            };

            // Unfiltered, because a thousand filtered frames converge on the filter's opinion and
            // not on the answer.
            const std::vector<float> reference = render(64, false);
            const std::vector<float> raw = render(0, false);
            const std::vector<float> filtered = render(0, true);

            const auto errorAgainstReference = [&](const std::vector<float>& values) {
                float squares = 0.0f;
                std::size_t counted = 0;
                for (std::size_t i = 1; i < values.size(); i += 4)
                {
                    // Only where there is a surface: the sky above the horizon is not being
                    // filtered and averages to itself, so counting it would dilute both figures.
                    if (values[i] == reference[i] && values[i] == 0.0f)
                        continue;

                    const float error = values[i] - reference[i];
                    squares += error * error;
                    ++counted;
                }

                return std::sqrt(squares / static_cast<float>(counted));
            };

            const float before = errorAgainstReference(raw);
            const float after = errorAgainstReference(filtered);

            EXPECT_GT(before, 0.02f) << "one sample is noisy enough here for the question to mean something";
            EXPECT_LT(after, before * 0.10f)
                << "and the filter takes most of that error away: " << before << " becomes " << after;

            // **And the temporal half, which is where the error actually falls.** The spatial cascade
            // borrows samples sideways from neighbours looking at the same surface, which has a
            // floor: there are only so many of those and they are correlated. Averaging over frames
            // borrows from samples that are genuinely independent, so its error keeps falling for as
            // long as the history is allowed to grow.
            //
            // Sequenced frames with no averaging in the composite: the sampler advances, so each
            // frame is a different draw, and the only thing combining them is the accumulator under
            // test. A still camera, so every pixel reprojects onto itself and no history is
            // rejected — which is the case this has to get right before any other.
            //
            // **`resetHistory` before each run, and leaving it out is what made this test lie.** The
            // fixture renders many frames, the accumulator keeps what it built across all of them,
            // and a "one frame" baseline taken without a reset is a baseline that already has a
            // history in it — which reads as the accumulator doing nothing at all.
            const auto renderSequence = [&](std::uint32_t frames) {
                std::vector<float> values;
                renderFiltered(scene, camera, size, values, frames);
                return values;
            };

            const std::vector<float> settledPixels = renderSequence(Shaders::ACCUMULATE_FRAMES);
            const float settled = errorAgainstReference(settledPixels);

            // **The accumulator may not make this worse, and on this surface that is the whole of
            // what it can be asked.** Measured here, the cascade alone already lands at 0.0020 of
            // the converged reference — a flat sheet under a smooth sky is precisely
            // where five levels of à-trous have every advantage, since the signal is uniform and
            // every neighbour is a valid sample of it. What the history is for is the case this
            // scene does not have: contact regions, small geometry, and pixels with few neighbours
            // looking at the same thing, which is what the test below this one is for.
            EXPECT_LE(settled, after * 1.02f)
                << "the history does not cost what the cascade gained: " << after << " becomes " << settled;

            // **And it converges toward the reference rather than toward its own opinion.** An
            // average that drifted would still be quieter, and quieter is not the claim: the mean of
            // the settled picture has to sit where the converged one does, or the accumulator is
            // dimming the frame and calling it denoising.
            double settledMean = 0.0;
            double referenceMean = 0.0;
            std::size_t counted = 0;
            for (std::size_t i = 1; i < settledPixels.size(); i += 4)
            {
                if (settledPixels[i] == reference[i] && settledPixels[i] == 0.0f)
                    continue;

                settledMean += static_cast<double>(settledPixels[i]);
                referenceMean += static_cast<double>(reference[i]);
                ++counted;
            }

            ASSERT_GT(counted, 0u);
            settledMean /= static_cast<double>(counted);
            referenceMean /= static_cast<double>(counted);

            EXPECT_NEAR(settledMean, referenceMean, referenceMean * 0.02)
                << "the accumulated mean is " << settledMean << " against a converged " << referenceMean;
        }

        /// What the history is worth where the cascade has nothing to borrow from.
        ///
        /// **The grazing sheet above is the wavelet's best case and cannot answer this.** Every pixel
        /// of it looks at one flat surface under one smooth sky, so every pixel has the *same*
        /// expected bounce — a hundred and twenty-five taps average a hundred and twenty-five draws
        /// from one distribution, and the error falls by the square root of that. It converges to
        /// within a third of a byte on a single frame, and a history cannot improve on nothing left
        /// to remove.
        ///
        /// So this is the other case, and it is the one Morrowind's geometry actually is: a surface
        /// whose neighbours disagree. The sheet is cut into a grid of coplanar cells whose *shading*
        /// normals alternate by forty degrees, which is far outside what `mNormalPower` lets a tap
        /// carry — so the plane test passes everywhere, the normal test rejects nearly every
        /// neighbour, and the cascade is left with little more than the centre pixel. Nothing about
        /// the accumulator changes: a still camera reprojects every pixel onto itself.
        ///
        /// The alternating tilt is not a trick to defeat the filter. It is what a bumpy surface is,
        /// and the reason the two neighbours may not be averaged is that they are genuinely lit
        /// differently — a cosine lobe tilted forty degrees samples a different part of this sky.
        TEST_F(RtxVisibilityTest, theHistoryCarriesWhereTheCascadeHasNoNeighboursToBorrow)
        {
            constexpr std::uint32_t size = 64;
            constexpr int cells = 16;
            constexpr float extent = 4000.0f;
            constexpr float tilt = 20.0f;

            std::vector<osg::Vec3f> positions;
            std::vector<osg::Vec3f> normals;
            std::vector<std::uint32_t> indices;
            for (int y = 0; y < cells; ++y)
                for (int x = 0; x < cells; ++x)
                {
                    const float lowX = -extent + 2.0f * extent * static_cast<float>(x) / cells;
                    const float highX = -extent + 2.0f * extent * static_cast<float>(x + 1) / cells;
                    const float lowY = -extent + 2.0f * extent * static_cast<float>(y) / cells;
                    const float highY = -extent + 2.0f * extent * static_cast<float>(y + 1) / cells;

                    const auto base = static_cast<std::uint32_t>(positions.size());
                    positions.emplace_back(lowX, lowY, 0.0f);
                    positions.emplace_back(highX, lowY, 0.0f);
                    positions.emplace_back(highX, highY, 0.0f);
                    positions.emplace_back(lowX, highY, 0.0f);

                    // Coplanar, so nothing here is a step in the geometry; only the normal moves.
                    const float lean = osg::DegreesToRadians((x + y) % 2 == 0 ? tilt : -tilt);
                    const osg::Vec3f leaning(std::sin(lean), 0.0f, std::cos(lean));
                    for (int corner = 0; corner < 4; ++corner)
                        normals.push_back(leaning);

                    for (const std::uint32_t offset : sQuadIndices)
                        indices.push_back(base + offset);
                }

            SceneDesc scene;
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(positions, normals, {}, indices) });

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -2600.0f, 2600.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

            // A sky that changes a great deal between the two tilts, so that two neighbouring cells
            // really do have different answers and averaging them really is wrong.
            camera.mSkyHorizon = osg::Vec3f(0.90f, 0.10f, 0.05f);
            camera.mSkyZenith = osg::Vec3f(0.05f, 0.20f, 0.90f);

            const auto renderSequence = [&](std::uint32_t frames) {
                std::vector<float> values;
                renderFiltered(scene, camera, size, values, frames);
                return values;
            };

            // Unfiltered, because a converged reference has to be the answer and not the filter's
            // opinion of it.
            std::vector<float> reference;
            renderRadiance(scene, camera, size, reference, 128, false);

            const auto errorAgainstReference = [&](const std::vector<float>& values) {
                double squares = 0.0;
                std::size_t counted = 0;
                for (std::size_t i = 0; i < values.size(); i += 4)
                    for (std::size_t channel = 0; channel < 3; ++channel)
                    {
                        const std::size_t at = i + channel;
                        // Only where the grid is: the sky around it is not being filtered.
                        if (values[at] == reference[at] && reference[at] == 0.0f)
                            continue;

                        const double error = static_cast<double>(values[at]) - static_cast<double>(reference[at]);
                        squares += error * error;
                        ++counted;
                    }

                return std::sqrt(squares / static_cast<double>(counted));
            };

            const std::vector<float> settledPixels = renderSequence(Shaders::ACCUMULATE_FRAMES);
            const double alone = errorAgainstReference(renderSequence(1));
            const double settled = errorAgainstReference(settledPixels);

            // Measured on this box through `Channel::Radiance`: 0.00380 against the converged
            // reference with the cascade alone, 0.00214 once sixteen frames are behind it — the
            // history removes **44%** of the error the filter cannot reach. Deterministic to the last
            // digit across runs, which is what lets the bounds below sit this close to the figures.
            //
            // **A third was the quantiser, and this is the figure without it.** Read back as bytes
            // the same pair came to 0.00406 and 0.00253, a ratio of 0.62 — because 0.00253 is two
            // thirds of one byte at this brightness and the settled frame was sitting on the floor
            // the output format put under it. The raw figure barely moved (0.00406 to 0.00380) and
            // the settled one moved a sixth, which is exactly the shape a quantiser's floor has: it
            // costs the quiet number and not the noisy one.
            EXPECT_GT(alone, 0.003) << "the cascade alone leaves enough error here for the question to mean something: "
                                    << alone;
            EXPECT_LT(settled, alone * 0.60)
                << "and a history of " << Shaders::ACCUMULATE_FRAMES << " frames takes two fifths of what the cascade "
                << "cannot: " << alone << " becomes " << settled;

            // **And it converges on the reference rather than on its own opinion.** Quieter is not
            // the claim — an average that drifted would be quieter too, and wrong.
            double settledMean = 0.0;
            double referenceMean = 0.0;
            std::size_t counted = 0;
            for (std::size_t i = 0; i < settledPixels.size(); i += 4)
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    const std::size_t at = i + channel;
                    if (settledPixels[at] == reference[at] && reference[at] == 0.0f)
                        continue;

                    settledMean += static_cast<double>(settledPixels[at]);
                    referenceMean += static_cast<double>(reference[at]);
                    ++counted;
                }

            ASSERT_GT(counted, 0u);
            settledMean /= static_cast<double>(counted);
            referenceMean /= static_cast<double>(counted);

            EXPECT_NEAR(settledMean, referenceMean, referenceMean * 0.02)
                << "the accumulated mean is " << settledMean << " against a converged " << referenceMean;
        }

        /// A floor meeting a wall, and the filter keeping them apart.
        ///
        /// **Everything else here would pass with a plain blur.** Smoothing noise and preserving a
        /// mean are what any average does; what makes this a denoiser rather than a soft-focus
        /// filter is that it refuses to mix two surfaces that happen to be neighbours on screen.
        ///
        /// So this measures the one place where that shows: the step from one row to the next
        /// across the crease. Away from it a blur is nearly harmless, because five levels of a
        /// B3 kernel put most of their weight near the centre however far the taps reach — which is
        /// exactly why a test comparing the two ends of the frame passes with the guide switched
        /// off, and this one does not.
        ///
        /// The crease is found rather than assumed: it is the row boundary where the unfiltered
        /// picture jumps hardest, which is where the geometry says it should be. The two surfaces
        /// are told apart by their normals alone, and lit differently for the same reason — a
        /// floor's cosine-weighted hemisphere is centred on the zenith and a wall's lies along the
        /// horizon, and this sky runs a long way between the two.
        TEST_F(RtxVisibilityTest, theFilterWillNotMixAFloorIntoTheWallStandingOnIt)
        {
            constexpr std::uint32_t size = 64;

            const std::array wall{
                osg::Vec3f(-2000.0f, 0.0f, 0.0f),
                osg::Vec3f(2000.0f, 0.0f, 0.0f),
                osg::Vec3f(2000.0f, 0.0f, 4000.0f),
                osg::Vec3f(-2000.0f, 0.0f, 4000.0f),
            };

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(makeSheet(4000.0f, 0.0f), {}, {}, sQuadIndices) });
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(wall, {}, {}, sQuadIndices) });

            // The floor fills the bottom of the frame and the wall the top, with the crease running
            // straight across the middle of it.
            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -1200.0f, 900.0f), osg::Vec3f(0.0f, 0.0f, 250.0f), 60.0f, size, size, 100000.0f);
            camera.mSkyHorizon = osg::Vec3f(0.20f, 0.15f, 0.60f);
            camera.mSkyZenith = osg::Vec3f(0.80f, 0.65f, 0.15f);

            // Green, where this sky has its widest range between the horizon and the zenith. A row
            // at a time, so that sixty-four pixels stand behind every number and the sampling noise
            // that is left cannot be mistaken for a step.
            const auto rowMeans = [&](std::uint32_t accumulate, bool filter) {
                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, {}, camera, size, pixels, SeaState{}, accumulate, filter), size * size);

                std::array<float, size> rows{};
                for (std::uint32_t y = 0; y < size; ++y)
                {
                    float sum = 0.0f;
                    for (std::uint32_t x = 0; x < size; ++x)
                        sum += decodeSrgb(pixels[(std::size_t{ y } * size + x) * 4 + 1]);

                    rows[y] = sum / size;
                }

                return rows;
            };

            // Where the crease is, off a converged frame rather than a noisy one. A single sample's
            // row means swing by more than the step does, so asking a noisy picture where its
            // biggest jump is answers with the loudest pixel and not with the geometry.
            const std::array<float, size> converged = rowMeans(64, false);

            std::uint32_t crease = 0;
            for (std::uint32_t y = 1; y < size; ++y)
                if (std::abs(converged[y] - converged[y - 1]) > std::abs(converged[crease + 1] - converged[crease]))
                    crease = y - 1;

            const float truth = std::abs(converged[crease + 1] - converged[crease]);
            const std::array<float, size> filtered = rowMeans(0, true);
            const float kept = std::abs(filtered[crease + 1] - filtered[crease]);

            ASSERT_GT(truth, 0.02f) << "the two surfaces have to part company for this to mean anything";
            EXPECT_GT(kept, truth * 0.7f) << "the step at row " << crease << " survives the filter: it is " << truth
                                          << " in the converged frame and " << kept << " in the filtered one";
        }

        /// A wall smaller than the frame leaves sky around it, and the count is the area it covers.
        ///
        /// The frame is 115.47 units tall at a hundred units, so a wall 60 units across covers
        /// 60 / 115.47 of the image in each direction: 0.5196 squared, which is 27.0% of 4096
        /// pixels — 1106 of them, give or take the pixels the edge falls inside.
        TEST_F(RtxVisibilityTest, aWallSmallerThanTheFrameCoversTheAreaItSubtends)
        {
            constexpr std::uint32_t size = 64;

            const std::array positions{
                osg::Vec3f(-30.0f, 0.0f, -30.0f),
                osg::Vec3f(30.0f, 0.0f, -30.0f),
                osg::Vec3f(30.0f, 0.0f, 30.0f),
                osg::Vec3f(-30.0f, 0.0f, 30.0f),
            };

            SceneDesc scene;
            const Index mesh = scene.addMesh(positions, {}, {}, sQuadIndices);
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh });

            const Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            std::vector<std::uint8_t> pixels;
            const std::uint32_t hits = countHits(scene, {}, camera, size, pixels);

            const float halfExtent = 100.0f * std::tan(osg::DegreesToRadians(30.0f));
            const float covered = 30.0f / halfExtent;
            const auto expected = static_cast<std::uint32_t>(covered * covered * size * size);

            // Within a pixel of edge on each side of a 33-pixel square.
            const double tolerance = 2.0 * static_cast<double>(covered) * size + 4.0;
            EXPECT_NEAR(static_cast<double>(hits), static_cast<double>(expected), tolerance);
        }
    }
}

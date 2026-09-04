#include "fixture.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Rtx::Testing
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

        TEST_F(RtxVisibilityTest, anOrthographicCameraSendsItsRaysParallelRatherThanThroughAnEye)
        {
            constexpr std::uint32_t size = 64;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(sheetAt(25.0f, -100.0f), {}, {}, sQuadIndices) });

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
            // `(0.18 / 0.481258)^0.75 = 0.478268`, so the frame reaches the display transform at
            // 0.239134 linear.
            //
            // The tone curve takes its shadow offset off that — 0.239134 is past three times it, so
            // the whole 0.04 comes off — and leaves the rest alone, being far under the compression
            // point. `1.055 * 0.199134^(1/2.4) - 0.055 = 0.483578`, or 123 of 255.
            std::vector<std::uint8_t> measured;
            renderPicture(makeWall(), {}, camera, size, measured);

            ASSERT_EQ(measured.size(), pixels.size());
            for (std::size_t i = 0; i < measured.size(); i += 4)
            {
                ASSERT_NEAR(measured[i], 123, 1) << "red at pixel " << i / 4;
                ASSERT_NEAR(measured[i + 1], 123, 1) << "green at pixel " << i / 4;
                ASSERT_NEAR(measured[i + 2], 123, 1) << "blue at pixel " << i / 4;
            }
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
            countHits(scene, {}, camera, size, hard, { .mFrames = 16 });

            std::vector<std::uint8_t> soft;
            countHits(scene, {}, camera, size, soft, { .mFrames = 16, .mJitter = true });

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
            constexpr std::size_t centre = centreOf(size);

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
            constexpr std::size_t centre = centreOf(size);

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

            const auto expected = [](float z) { return far / (far - near) * (1.0f - near / z); };

            // Two floats a pixel: clip depth, then distance from the eye.
            constexpr std::size_t stride = 2;
            constexpr std::size_t centre = centreOf(size) * stride;
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

        /// A mesh whose pose changed is traced against the vertices the device computed from it,
        /// without a scene rebuild.
        ///
        /// **What a skinned body needs and moving an instance cannot give.** A crate that moves says
        /// so with its transform; an arm that swings does not — the actor's transform is where the
        /// actor stands, and the pose lives in vertices underneath it. So this wall stays at the
        /// identity throughout and only its one bone is written again: the skinning pass has to
        /// compute the corners from the bind pose and the refit has to follow them, and a
        /// `placeScene` that rebuilt the top level over an untouched bottom level would trace the
        /// first wall every time and read the first distance.
        ///
        /// The distance is the assertion rather than the hit count, because it names *where* the
        /// new triangles are and not merely that something changed. Its 1.0000814 is the centre
        /// pixel's own half-pixel offset from the view axis, worked out in the depth test above.
        TEST_F(RtxVisibilityTest, aDeformedMeshIsTracedAgainstItsNewVerticesWithoutRebuildingTheScene)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::size_t centre = centreOf(size) * 2 + 1;
            constexpr float far = 100000.0f;
            constexpr float centreCosine = 1.0000814f;

            const Shaders::VisibilityConstants camera
                = makeCamera(osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f), 60.0f, size, size, far);

            SceneDesc scene;
            const Index wall
                = scene.addMesh(wallAt(200.0f), {}, {}, sQuadIndices, {}, Deform::Rig, addOneBoneRig(scene, 4));
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = wall });
            poseByOneBone(scene, wall, osg::Matrixf::identity());

            std::vector<std::uint8_t> pixels;
            ASSERT_EQ(countHits(scene, {}, camera, size, pixels), size * size);

            std::vector<float> depth;
            mRenderer->readChannel(Channel::Depth, depth);
            ASSERT_NEAR(depth[centre], 200.0f * centreCosine, 0.2f) << "where it was built";

            /// Moves the wall's bone `away` units off its bind pose and replaces the scene's
            /// placement, exactly as a frame of the game does: clear, re-walk, hand it back.
            const auto deformTo = [&](float away) {
                scene.clearPlacement();
                poseByOneBone(scene, wall, osg::Matrixf::translate(0.0f, away - 200.0f, 0.0f));
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = wall });
                mRenderer->placeScene(Rtx::sWorld, scene, SeaState{});
            };

            deformTo(400.0f);
            mRenderer->renderFrame(camera, FrameOptions{});
            EXPECT_EQ(mRenderer->finishFrame().value().mHits, size * size);

            mRenderer->readChannel(Channel::Depth, depth);
            EXPECT_NEAR(depth[centre], 400.0f * centreCosine, 0.4f) << "and the structure followed its vertices";

            // Behind the eye, where a wall that was never rebuilt would still be filling the frame.
            deformTo(-1000.0f);
            mRenderer->renderFrame(camera, FrameOptions{});
            EXPECT_EQ(mRenderer->finishFrame().value().mHits, 0u);
        }
    }
}

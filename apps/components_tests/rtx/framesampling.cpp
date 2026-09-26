#include <cstdint>

#include <gtest/gtest.h>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/camera.hpp>
#include <components/rtx/frameoptions.hpp>
#include <components/rtx/framesampling.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/visibility.h>

namespace Rtx
{
    namespace
    {
        /// A statement as a builder leaves it: a camera and the frame's index, and nothing sampled.
        Shaders::VisibilityConstants stated()
        {
            Shaders::VisibilityConstants frame{};
            frame.mCamera.mRight = osg::Vec3f(2.0f, 0.0f, 0.0f);
            frame.mCamera.mUp = osg::Vec3f(0.0f, 0.0f, 1.0f);
            frame.mArms = frame.mCamera;
            frame.mArms.mRight = osg::Vec3f(3.0f, 0.0f, 0.0f);
            frame.mOrigin = osg::Vec3f(10.0f, 20.0f, 30.0f);
            frame.mFrame = 4;
            frame.mRayMask = Shaders::MASK_FIRST_PERSON;
            return frame;
        }

        /// **The profile answers what the frame did not ask**, the delight and the surface view
        /// among them, which the host once read back off the renderer to hand in again — and a
        /// frame that forgot traced with a delight of nought.
        TEST(RtxFrameSamplingTest, theProfileAnswersWhatTheFrameDidNotAsk)
        {
            const RenderProfile profile{ .mDelight = 0.75f, .mShow = SurfaceView::Albedo };

            const Shaders::VisibilityConstants played
                = sampleFrame(stated(), FrameOptions{}, profile, Reconstruction{}, InstanceCounts{}, nullptr);
            EXPECT_EQ(played.mDelight, 0.75f);
            EXPECT_EQ(played.mShow, static_cast<std::uint32_t>(SurfaceView::Albedo));

            // And the frame's own ask stands over it, which is how a test of one input asks for it.
            const Shaders::VisibilityConstants asked
                = sampleFrame(stated(), FrameOptions{ .mDelight = 0.0f, .mShow = SurfaceView::Shaded }, profile,
                    Reconstruction{}, InstanceCounts{}, nullptr);
            EXPECT_EQ(asked.mDelight, 0.0f);
            EXPECT_EQ(asked.mShow, static_cast<std::uint32_t>(SurfaceView::Shaded));
        }

        /// Every field the sampling writes comes from the sampling, and each from the input that
        /// decides it.
        ///
        /// By hand: the reconstruction's jitter is Halton's at the frame's index plus one, bases two
        /// and three, centred — index five is (1/2 + 1/8, 2/3 + 1/9) less a half, (0.125, 0.27778). The
        /// arms' plane is three wide against the eye's two, so the spread is 1.5 across and one up.
        /// The eye moved from (7, 20, 30) to (10, 20, 30), three along x.
        TEST(RtxFrameSamplingTest, everySampledFieldComesFromWhatDecidesIt)
        {
            const Reconstruction jittering{ .mJitter = true, .mNoise = NoiseSource::WhiteHash, .mLevelBias = -0.5f };
            const InstanceCounts counts{ .mFirstPerson = 1 };
            Shaders::VisibilityConstants previous = stated();
            previous.mOrigin = osg::Vec3f(7.0f, 20.0f, 30.0f);
            previous.mCamera.mForward = osg::Vec3f(0.0f, 1.0f, 0.0f);

            const Shaders::VisibilityConstants sampled
                = sampleFrame(stated(), FrameOptions{}, RenderProfile{}, jittering, counts, &previous);

            EXPECT_EQ(sampled.mCamera.mJitter, haltonJitter(4));
            EXPECT_FLOAT_EQ(sampled.mCamera.mJitter.x(), 0.125f);
            EXPECT_NEAR(sampled.mCamera.mJitter.y(), 7.0f / 9.0f - 0.5f, 1e-6f);
            EXPECT_EQ(sampled.mArms.mJitter, sampled.mCamera.mJitter) << "the arms sample where the eye does";
            EXPECT_EQ(sampled.mNoise, Shaders::NOISE_WHITE_HASH);
            EXPECT_EQ(sampled.mLevelBias, -0.5f);
            EXPECT_EQ(sampled.mArmsSpread, osg::Vec2f(1.5f, 1.0f));
            EXPECT_EQ(sampled.mArmsInFrame, 1u);
            EXPECT_EQ(sampled.mCameraMotion, osg::Vec3f(3.0f, 0.0f, 0.0f));
            EXPECT_EQ(sampled.mPreviousForward, osg::Vec3f(0.0f, 1.0f, 0.0f));

            // What the statement carried passes through untouched.
            EXPECT_EQ(sampled.mOrigin, stated().mOrigin);
            EXPECT_EQ(sampled.mFrame, 4u);

            // A picture has no frame before it and moves nothing, and a reconstruction that does
            // not jitter samples the centre unless the frame states an offset of its own.
            const Shaders::VisibilityConstants picture
                = sampleFrame(stated(), FrameOptions{}, RenderProfile{}, Reconstruction{}, counts, nullptr);
            EXPECT_EQ(picture.mCamera.mJitter, osg::Vec2f());
            EXPECT_EQ(picture.mCameraMotion, osg::Vec3f());
            EXPECT_EQ(picture.mNoise, Shaders::NOISE_BLUE_TILE);

            const Shaders::VisibilityConstants offset = sampleFrame(stated(),
                FrameOptions{ .mJitter = osg::Vec2f(0.25f, 0.0f) }, RenderProfile{}, Reconstruction{}, counts, nullptr);
            EXPECT_EQ(offset.mCamera.mJitter, osg::Vec2f(0.25f, 0.0f));
        }
    }
}

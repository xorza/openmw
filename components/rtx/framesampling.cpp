#include "framesampling.hpp"

#include <cassert>
#include <cstdint>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include "camera.hpp"
#include "frameoptions.hpp"
#include "mesh.hpp"
#include "shaders/scene.h"

namespace Rtx
{
    namespace
    {
        /// How much wider the arms' image plane is than the eye's, per axis —
        /// `VisibilityConstants::mArmsSpread`.
        osg::Vec2f armsSpreadOf(const Shaders::VisibilityConstants& frame)
        {
            return osg::Vec2f(frame.mArms.mRight.length() / frame.mCamera.mRight.length(),
                frame.mArms.mUp.length() / frame.mCamera.mUp.length());
        }

        /// Whether every field `sampleFrame` writes is still what a builder leaves it: nought.
        [[maybe_unused]] bool leavesSamplingAlone(const Shaders::VisibilityConstants& stated)
        {
            return stated.mCamera.mJitter == osg::Vec2f() && stated.mArms.mJitter == osg::Vec2f() && stated.mNoise == 0u
                && stated.mLevelBias == 0.0f && stated.mArmsSpread == osg::Vec2f() && stated.mMediumInFrame == 0u
                && stated.mAdditiveInFrame == 0u && stated.mArmsInFrame == 0u && stated.mCameraMotion == osg::Vec3f()
                && stated.mPreviousForward == osg::Vec3f() && stated.mPreviousRight == osg::Vec3f()
                && stated.mPreviousUp == osg::Vec3f() && stated.mDelight == 0.0f && stated.mShow == 0u;
        }
    }

    Shaders::VisibilityConstants sampleFrame(const Shaders::VisibilityConstants& stated, const FrameOptions& options,
        const RenderProfile& profile, const Reconstruction& reconstruction, const InstanceCounts& counts,
        const Shaders::VisibilityConstants* previous)
    {
        assert(leavesSamplingAlone(stated) && "a frame stated a field its sampling writes; ask through FrameOptions");
        assert(!(reconstruction.mJitter && options.mJitter.has_value())
            && "a frame asked for a fixed offset where the reconstruction walks its own sequence");

        Shaders::VisibilityConstants sampled = stated;

        // Where in the pixel this frame samples. Here rather than by the caller because the
        // sequence belongs to the frame index, which the renderer walks.
        sampled.mCamera.mJitter
            = reconstruction.mJitter ? haltonJitter(stated.mFrame) : options.mJitter.value_or(osg::Vec2f());

        // The two consequences of the reconstruction the trace reads for itself: where its draws
        // come from, and how far the shown pixel narrows every texture level. A picture's
        // `Reconstruction{}` says the tile and nought.
        sampled.mNoise
            = reconstruction.mNoise == NoiseSource::WhiteHash ? Shaders::NOISE_WHITE_HASH : Shaders::NOISE_BLUE_TILE;
        sampled.mLevelBias = reconstruction.mLevelBias;

        sampled.mDelight = options.mDelight.value_or(profile.mDelight);
        sampled.mShow = static_cast<std::uint32_t>(options.mShow.value_or(profile.mShow));

        // The arms' eye samples where the world's does, or the two halves of one frame would be
        // reconstructed from two grids.
        sampled.mArms.mJitter = sampled.mCamera.mJitter;
        sampled.mArmsSpread = armsSpreadOf(stated);

        // The scene's answer and not the camera's: a cell with no cloud in it has nothing for the
        // medium walk to find, wherever it is looked at from. The arms are the scene's and the
        // camera's both: a map draws none.
        sampled.mMediumInFrame = counts.mMedium > 0 ? 1 : 0;
        sampled.mAdditiveInFrame = counts.mAdditive > 0 ? 1 : 0;
        sampled.mArmsInFrame = counts.mFirstPerson > 0 && (stated.mRayMask & Shaders::MASK_FIRST_PERSON) != 0 ? 1 : 0;

        // The one subtraction of two world points, and it happens here. Two camera positions a
        // step apart subtract exactly in a float; the same difference taken on the device, between
        // coordinates six figures long, would be rounding. A picture has no last frame.
        if (previous != nullptr)
        {
            sampled.mCameraMotion = stated.mOrigin - previous->mOrigin;
            sampled.mPreviousForward = previous->mCamera.mForward;
            sampled.mPreviousRight = previous->mCamera.mRight;
            sampled.mPreviousUp = previous->mCamera.mUp;
        }

        return sampled;
    }
}

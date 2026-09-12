#include "offscreentrace.hpp"

#include <algorithm>
#include <cstddef>
#include <numbers>

#include <osg/FrameStamp>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>

#include <components/resource/imagemanager.hpp>

#include "camera.hpp"
#include "colour.hpp"
#include "posecull.hpp"
#include "poseupdate.hpp"
#include "scenedesc.hpp"
#include "sceneextractor.hpp"

namespace Rtx
{
    namespace
    {
        osg::Vec3f irradianceOf(const osg::Vec4f& colour)
        {
            return decodeColour(colour) * std::numbers::pi_v<float>;
        }
    }

    OffscreenTrace::OffscreenTrace(Renderer& renderer, const ViewRequest& request)
        : mRenderer(renderer)
        , mWidth(request.mWidth)
        , mHeight(request.mHeight)
        , mRowOrder(request.mRowOrder)
        , mRayMask(request.mRayMask)
        , mFraming(request.mFraming)
        , mAmbient(irradianceOf(request.mLight.mAmbient))
        , mTransparent(request.mClear.a() < 1.f)
    {
        mSun.mPosition = request.mLight.mDirection;
        if (mSun.mPosition.length2() > 0.f)
            mSun.mPosition.normalize();
        mSun.mIrradiance = irradianceOf(request.mLight.mDiffuse);

        mOptions.mWidth = request.mWidth;
        mOptions.mHeight = request.mHeight;
        mOptions.mClear = { request.mClear.r(), request.mClear.g(), request.mClear.b(), request.mClear.a() };
        mOptions.mScene = SceneSlot::world();

        if (request.mSubject == nullptr)
            return;

        mSubject = std::make_unique<Subject>();

        Subject& held = *mSubject;
        held.mNode = request.mSubject;
        held.mScene = std::make_unique<SceneDesc>();
        held.mUpdate = std::make_unique<PoseUpdate>();
        held.mPose = std::make_unique<PoseCull>();
        held.mPoseStamp = new osg::FrameStamp;
        held.mSlot = renderer.addViewScene();

        held.mExtractor = std::make_unique<SceneExtractor>(*held.mScene, request.mTraversals);
        held.mExtractor->setTraversalMask(request.mSubjectMask);
        held.mPose->setFrameStamp(held.mPoseStamp);

        mOptions.mScene = held.mSlot;
    }

    OffscreenTrace::Subject::~Subject() = default;

    OffscreenTrace::~OffscreenTrace()
    {
        if (mSubject != nullptr)
            mRenderer.dropViewScene(mSubject->mSlot);
    }

    const SceneDesc* OffscreenTrace::getScene() const
    {
        return mSubject != nullptr ? mSubject->mScene.get() : nullptr;
    }

    void OffscreenTrace::setView(const osg::Matrixf& view)
    {
        mView = view;
    }

    void OffscreenTrace::setExtent(std::uint32_t width, std::uint32_t height)
    {
        mOptions.mWidth = std::clamp(width, 1u, mWidth);
        mOptions.mHeight = std::clamp(height, 1u, mHeight);
    }

    Shaders::VisibilityConstants OffscreenTrace::describeCamera() const
    {
        const auto* perspective = std::get_if<SceneUtil::Perspective>(&mFraming.mProjection);
        Shaders::VisibilityConstants camera = perspective != nullptr
            ? makeCameraFromView(
                mView, perspective->mFieldOfView, mOptions.mWidth, mOptions.mHeight, mFraming.mNear, mFraming.mFar)
            : makeOrthographicCameraFromView(mView, std::get<SceneUtil::Orthographic>(mFraming.mProjection).mWidth,
                std::get<SceneUtil::Orthographic>(mFraming.mProjection).mHeight, mOptions.mWidth, mOptions.mHeight,
                mFraming.mNear, mFraming.mFar);

        // `ViewRequest::mRowOrder` says why the GUI's copy comes out the other way up.
        if (mRowOrder == RowOrder::BottomFirst)
            camera.mCamera.mUp = -camera.mCamera.mUp;

        camera.mSunPosition = mSun.mPosition;
        camera.mSunIrradiance = mSun.mIrradiance;
        camera.mAmbient = mAmbient;
        camera.mTransparentBackground = mTransparent ? 1 : 0;
        camera.mRayMask = mRayMask;

        return camera;
    }

    bool OffscreenTrace::rebuildSubject(
        const osg::FrameStamp& posing, std::size_t worldFrame, Resource::ImageManager& images)
    {
        if (mSubject == nullptr)
            return true;

        // Posed here, because nothing else will. The camera callback the game hangs on a doll's
        // subtree is what finds the head to look at, and it runs in an update traversal — and a
        // subtree that is in no graph is reached by no traversal but this one.
        Subject& subject = *mSubject;
        subject.mPosedFrame = static_cast<unsigned int>(posing.getFrameNumber());

        subject.mUpdate->reset();

        // `osg::NodeVisitor::setFrameStamp` takes a mutable pointer and stores it without writing
        // through it, which is the whole of why this is cast.
        subject.mUpdate->setFrameStamp(const_cast<osg::FrameStamp*>(&posing));
        subject.mUpdate->setTraversalNumber(subject.mPosedFrame);
        subject.mNode->accept(*subject.mUpdate);

        // Kept for `pick`, whose cull reads a clock of its own: the caller's stamp is the caller's
        // to reuse the moment this returns.
        *subject.mPoseStamp = posing;

        // Re-walked and not rebuilt, which the identity maps owning their keys is what makes
        // sound. Between one redraw and the next this subject is taken apart —
        // `NpcAnimation::updateParts` frees the body parts that changed and builds their
        // replacements — and the allocator is free to put a new part exactly where a retired one
        // was. A map keyed on the bare address found the retired part's entry under the new part's
        // and mirrored the wrong geometry, which is the torn figure a change of clothes produced; a
        // map that holds its key cannot be shown that address at all until it lets go.
        //
        // The placements are the one thing a redraw throws away, as the world's frame does: what a
        // walk refills wholesale goes, and the meshes and materials stay because they are what the
        // walk is trying not to read again.
        subject.mScene->clearPlacement();

        // The world's frame and not a redraw count. The number handed to `extract` picks which
        // of a `SceneUtil::LightSource`'s two buffers to read, which is a property of the frame the
        // world is in. The pose the walk reads is what the update above left in the bones, and it
        // is handed to the device as rows: no cull runs here and no traversal number gates it.
        subject.mExtractor->extract(*subject.mNode, osg::Matrixf::identity(), 0, worldFrame);

        // No `advance` between them, unlike the world's frame: a picture drawn when the subject
        // changes rather than when the frame does has no motion to describe, and `SceneDesc` answers
        // a scene that has never advanced with a previous transform equal to its current one — which
        // is the right answer here and a stale one otherwise.
        //
        // The sweep is what takes the parts that came off. It is sound for the same reason it is
        // sound for the world: this walk is the whole of what this picture is of.
        subject.mExtractor->retire();

        // It consumes the arrivals, so nothing here clears them.
        subject.mUploader.hand(
            mRenderer, SceneUploader::Handing{ .mSlot = subject.mSlot, .mScene = *subject.mScene, .mImages = images });

        return subject.mScene->placements().getPlacedCount() > 0;
    }

    void OffscreenTrace::traceInto(const GuiSlot texture, const bool readBack)
    {
        GuiTraceOptions options = mOptions;
        options.mReadBack = readBack;
        mRenderer.traceGuiTexture(texture, describeCamera(), options);
    }

    bool OffscreenTrace::pick(float x, float y, osg::NodePath& hit) const
    {
        if (mSubject == nullptr)
            return false;

        Subject& subject = *mSubject;
        const Shaders::VisibilityConstants camera = describeCamera();
        const osg::Vec3f direction = camera.mCamera.mForward + camera.mCamera.mRight * x - camera.mCamera.mUp * y;

        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector
            = new osgUtil::LineSegmentIntersector(osgUtil::Intersector::MODEL,
                camera.mOrigin + direction * mFraming.mNear, camera.mOrigin + direction * mFraming.mFar);
        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);

        // Posed here, on the processor, because the intersection reads the drawable's own copy.
        // `SceneUtil::RigGeometry` and `MorphGeometry` skin inside a cull traversal and answer an
        // intersection with whatever the last cull wrote; the picture was traced from a pose the
        // device computed, so without this the click would land on the bind pose. A number from the
        // shared sequence, because both deforming geometries refuse to move for one they have seen.
        const unsigned int posed = subject.mExtractor->getTraversals().next();
        subject.mPose->setTraversalNumber(posed);
        subject.mPoseStamp->setFrameNumber(posed);
        subject.mNode->accept(*subject.mPose);

        osgUtil::IntersectionVisitor visitor(intersector);
        visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);

        // The number the pose was written at, so a skinned mesh hands over the buffer that cull
        // wrote rather than the one it will be posed into next.
        visitor.setTraversalNumber(posed);

        subject.mNode->accept(visitor);

        if (!intersector->containsIntersections())
            return false;

        hit = intersector->getFirstIntersection().nodePath;
        return true;
    }
}

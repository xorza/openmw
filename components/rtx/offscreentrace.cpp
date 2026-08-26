#include "offscreentrace.hpp"

#include <algorithm>
#include <cstddef>
#include <numbers>

#include <osg/FrameStamp>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>
#include <osgUtil/UpdateVisitor>

#include <components/resource/imagemanager.hpp>

#include "camera.hpp"
#include "scenedesc.hpp"
#include "sceneextractor.hpp"

namespace Rtx
{
    namespace
    {
        osg::Vec3f irradianceOf(const osg::Vec4f& colour)
        {
            return osg::Vec3f(colour.r(), colour.g(), colour.b()) * std::numbers::pi_v<float>;
        }
    }

    OffscreenTrace::OffscreenTrace(Renderer& renderer, std::uint32_t width, std::uint32_t height)
        : mRenderer(renderer)
        , mWidth(width)
        , mHeight(height)
    {
        mOptions.mWidth = width;
        mOptions.mHeight = height;
        mOptions.mScene = sWorld;

        // A picture inside the interface is traced when its subject changes and not per frame, so
        // there is no later frame for an unfinished bake to be drained into.
        mUploader.setStaged(true);
    }

    OffscreenTrace::OffscreenTrace(Renderer& renderer, std::uint32_t width, std::uint32_t height, osg::Node& subject,
        osg::Node::NodeMask mask, Traversals* traversals)
        : mRenderer(renderer)
        , mSubject(&subject)
        , mScene(std::make_unique<SceneDesc>())
        , mUpdate(std::make_unique<osgUtil::UpdateVisitor>())
        , mViewScene(renderer.addViewScene())
        , mWidth(width)
        , mHeight(height)
    {
        mExtractor = std::make_unique<SceneExtractor>(*mScene, traversals);
        mExtractor->setTraversalMask(mask);

        mOptions.mWidth = width;
        mOptions.mHeight = height;
        mOptions.mScene = mViewScene;
    }

    OffscreenTrace::~OffscreenTrace()
    {
        if (mSubject != nullptr)
            mRenderer.dropViewScene(mViewScene);
    }

    void OffscreenTrace::setPerspective(float fieldOfView, float near, float far)
    {
        mPerspective = true;
        mFieldOfView = fieldOfView;
        mNear = near;
        mFar = far;
    }

    void OffscreenTrace::setOrthographic(float width, float height, float near, float far)
    {
        mPerspective = false;
        mBoxWidth = width;
        mBoxHeight = height;
        mNear = near;
        mFar = far;
    }

    void OffscreenTrace::setLight(const osg::Vec3f& towardsSun, const osg::Vec4f& diffuse, const osg::Vec4f& ambient)
    {
        mSunPosition = towardsSun;
        if (mSunPosition.length2() > 0.f)
            mSunPosition.normalize();

        mSunIrradiance = irradianceOf(diffuse);
        mAmbient = irradianceOf(ambient);
    }

    void OffscreenTrace::setClearColour(const osg::Vec4f& colour)
    {
        mOptions.mClear = { colour.r(), colour.g(), colour.b(), colour.a() };
        mTransparent = colour.a() < 1.f;
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
        Shaders::VisibilityConstants camera = mPerspective
            ? makeCameraFromView(mView, mFieldOfView, mOptions.mWidth, mOptions.mHeight, mNear, mFar)
            : makeOrthographicCameraFromView(
                  mView, mBoxWidth, mBoxHeight, mOptions.mWidth, mOptions.mHeight, mNear, mFar);

        // `setRowOrder` says why the GUI's copy comes out the other way up.
        if (mRowOrder == RowOrder::BottomFirst)
            camera.mCamera.mUp = -camera.mCamera.mUp;

        camera.mSunPosition = mSunPosition;
        camera.mSunIrradiance = mSunIrradiance;
        camera.mAmbient = mAmbient;
        camera.mTransparentBackground = mTransparent ? 1 : 0;

        return camera;
    }

    bool OffscreenTrace::rebuildSubject(
        const osg::FrameStamp& posing, std::size_t worldFrame, Resource::ImageManager& images)
    {
        if (mSubject == nullptr)
            return true;

        // **Posed here, because nothing else will.** The camera callback the game hangs on a doll's
        // subtree is what finds the head to look at, and it runs in an update traversal — and a
        // subtree that is in no graph is reached by no traversal but this one.
        mPosedFrame = static_cast<unsigned int>(posing.getFrameNumber());

        mUpdate->reset();

        // `osg::NodeVisitor::setFrameStamp` takes a mutable pointer and stores it without writing
        // through it, which is the whole of why this is cast.
        mUpdate->setFrameStamp(const_cast<osg::FrameStamp*>(&posing));
        mUpdate->setTraversalNumber(mPosedFrame);
        mSubject->accept(*mUpdate);

        // **Re-walked and not rebuilt**, which the identity maps owning their keys is what makes
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
        mScene->clearPlacement();

        // **A clock that reads differently every time.** The walk poses the subject by running a
        // cull traversal over it, and `SceneUtil::Skeleton` and both deforming geometries refuse to
        // move for a traversal number they have already seen. A walk that said zero every time
        // skinned the doll when the inventory first opened and never again.
        //
        // **The world's frame and not a redraw count.** The number handed to `extract` picks which
        // of a `SceneUtil::LightSource`'s two buffers to read, which is a property of the frame the
        // world is in; what says "pose again" is the traversal number above.
        mExtractor->extract(*mSubject, osg::Matrixf::identity(), 0, worldFrame);

        // **No `advance` between them**, unlike the world's frame: a picture drawn when the subject
        // changes rather than when the frame does has no motion to describe, and `SceneDesc` answers
        // a scene that has never advanced with a previous transform equal to its current one — which
        // is the right answer here and a stale one otherwise.
        //
        // The sweep is what takes the parts that came off. It is sound for the same reason it is
        // sound for the world: this walk is the whole of what this picture is of.
        mExtractor->retire();

        // It consumes the arrivals, so nothing here clears them.
        mUploader.hand(mRenderer, mViewScene, *mScene, images);

        return mScene->getPlacedCount() > 0;
    }

    void OffscreenTrace::traceInto(std::uint32_t texture)
    {
        mRenderer.traceGuiTexture(texture, describeCamera(), mOptions);
    }

    bool OffscreenTrace::pick(float x, float y, osg::NodePath& hit) const
    {
        if (mSubject == nullptr)
            return false;

        const Shaders::VisibilityConstants camera = describeCamera();
        const osg::Vec3f direction = camera.mCamera.mForward + camera.mCamera.mRight * x - camera.mCamera.mUp * y;

        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector = new osgUtil::LineSegmentIntersector(
            osgUtil::Intersector::MODEL, camera.mOrigin + direction * mNear, camera.mOrigin + direction * mFar);
        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);

        osgUtil::IntersectionVisitor visitor(intersector);
        visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);

        // The frame the pose was written for, so a skinned mesh hands over the buffer the picture was
        // made from rather than the one it will be posed into next.
        visitor.setTraversalNumber(mPosedFrame);

        mSubject->accept(visitor);

        if (!intersector->containsIntersections())
            return false;

        hit = intersector->getFirstIntersection().nodePath;
        return true;
    }
}

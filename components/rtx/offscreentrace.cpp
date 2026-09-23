#include "offscreentrace.hpp"

#include <algorithm>
#include <cstddef>
#include <numbers>
#include <optional>
#include <variant>

#include <osg/CullSettings>
#include <osg/FrameStamp>
#include <osg/Matrix>
#include <osg/Matrixd>
#include <osg/NodeVisitor>
#include <osg/Transform>
#include <osg/Viewport>
#include <osgUtil/CullVisitor>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

#include <components/resource/imagemanager.hpp>

#include "camera.hpp"
#include "colour.hpp"
#include "nodekind.hpp"
#include "poseupdate.hpp"
#include "scenedesc.hpp"
#include "sceneextractor.hpp"
#include "shaders/visibility.h"
#include "slot.hpp"

namespace Rtx
{
    /// A cull traversal that culls nothing and draws nothing, for the one thing left that answers
    /// only to an `osgUtil::CullVisitor`: `SceneUtil::RigGeometry` and `SceneUtil::MorphGeometry`
    /// skin inside `accept` by casting the visitor to one, and `OffscreenTrace::pick` reads that
    /// posed copy once per click. Not for a whole graph: `Terrain::TerrainDrawable::cull` puts the
    /// chunk in a render bin and never applies it. A real `CullVisitor`, because those casts are
    /// unchecked. Whoever uses it owes it a frame stamp — `SceneUtil::FrameTimeSource` reads the
    /// simulation time off it unchecked — and a traversal number a skeleton has not seen.
    class PoseCull : public osgUtil::CullVisitor
    {
    public:
        PoseCull()
        {
            setCullingMode(osg::CullSettings::NO_CULLING);
            setStateGraph(new osgUtil::StateGraph);

            // Nothing will ever be drawn out of it, but `accept` pushes the drawable's own state set
            // on the way past, and a state set naming a render bin sends the visitor to
            // `_currentRenderBin` — which a good deal of Morrowind's content names, the error marker
            // a missing model resolves to among them.
            setRenderStage(new osgUtil::RenderStage);

            // `CullStack` reads the back of each of these without checking, so they are pushed once
            // and never popped: an empty stack is not a permissive one, it is a crash.
            pushViewport(new osg::Viewport(0, 0, 1, 1));
            pushProjectionMatrix(new osg::RefMatrix);
            pushModelViewMatrix(new osg::RefMatrix, osg::Transform::ABSOLUTE_RF);
        }

        /// The pose is read off the drawable afterwards, so there is nothing to do with it here.
        void apply(osg::Drawable&) override {}

        /// Everything but a particle simulation, which a real cull visitor would otherwise run:
        /// `osgParticle` keeps a once-per-frame guard and a `_t0` per processor, so two visitors
        /// on two clocks difference a `_t0` from one against a time from the other.
        /// `SceneExtractor` owns the one emitter clock in this renderer.
        void apply(osg::Node& node) override
        {
            const NodeKind kind = mKinds.of(node);
            if (kind == NodeKind::ParticleProcessor || kind == NodeKind::ParticleUpdater)
                return;

            osgUtil::CullVisitor::apply(node);
        }

    private:
        NodeKinds mKinds;
    };

    namespace
    {
        osg::Vec3f irradianceOf(const osg::Vec4f& colour)
        {
            return decodeColour(colour) * std::numbers::pi_v<float>;
        }
    }

    OffscreenTrace::OffscreenTrace(Renderer& renderer, const ViewRequest& request)
        : mRenderer(renderer)
        , mRequest(request)
        , mExtentWidth(request.mWidth)
        , mExtentHeight(request.mHeight)
    {
        if (request.mSubject == nullptr)
            return;

        mSubject = std::make_unique<Subject>();

        Subject& held = *mSubject;
        held.mNode = request.mSubject;
        held.mScene = std::make_unique<SceneDesc>();
        held.mUpdate = std::make_unique<PoseUpdate>();
        held.mPose = std::make_unique<PoseCull>();
        held.mPoseStamp = new osg::FrameStamp;
        held.mSlot = ViewScene(renderer);

        held.mExtractor = std::make_unique<SceneExtractor>(*held.mScene, request.mTraversals);
        held.mExtractor->setTraversalMask(request.mSubjectMask);
        held.mPose->setFrameStamp(held.mPoseStamp);
    }

    OffscreenTrace::Subject::~Subject() = default;

    OffscreenTrace::~OffscreenTrace() = default;

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
        mExtentWidth = std::clamp(width, 1u, mRequest.mWidth);
        mExtentHeight = std::clamp(height, 1u, mRequest.mHeight);
    }

    std::optional<Shaders::VisibilityConstants> OffscreenTrace::describeCamera() const
    {
        const SceneUtil::Framing& framing = mRequest.mFraming;
        const auto* perspective = std::get_if<SceneUtil::Perspective>(&framing.mProjection);
        std::optional<Shaders::VisibilityConstants> camera = perspective != nullptr
            ? makeCameraFromView(
                mView, perspective->mFieldOfView, mExtentWidth, mExtentHeight, framing.mNear, framing.mFar)
            : makeOrthographicCameraFromView(mView, std::get<SceneUtil::Orthographic>(framing.mProjection).mWidth,
                std::get<SceneUtil::Orthographic>(framing.mProjection).mHeight, mExtentWidth, mExtentHeight,
                framing.mNear, framing.mFar);
        if (!camera.has_value())
            return std::nullopt;

        // `ViewRequest::mRowOrder` says why the GUI's copy comes out the other way up.
        if (mRequest.mRowOrder == RowOrder::BottomFirst)
            camera->mCamera.mUp = -camera->mCamera.mUp;

        // Where the light stands, unit, in the sense `ViewRequest::mLight` states it and the
        // trace takes it.
        osg::Vec3f sun = mRequest.mLight.mDirection;
        if (sun.length2() > 0.f)
            sun.normalize();

        camera->mSun = Shaders::sunSource(sun, irradianceOf(mRequest.mLight.mDiffuse));
        camera->mAmbient = irradianceOf(mRequest.mLight.mAmbient);
        camera->mTransparentBackground = mRequest.mClear.a() < 1.f ? 1 : 0;
        camera->mRayMask = mRequest.mRayMask;
        describeTexturing(mRenderer.getProfile(), *camera);

        return camera;
    }

    bool OffscreenTrace::rebuildSubject(const osg::FrameStamp& posing, Resource::ImageManager& images)
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

        // The picture's own eye, for whatever in the subject turns to face one.
        subject.mExtractor->setEye(viewBasisOf(osg::Matrixd::inverse(osg::Matrixd(mView))));

        // The number the update above ran at, because that is what a semi-active skeleton compares
        // the walk's `markReached` against: a skeleton told another number stops moving its bones
        // three redraws on. The pose the walk reads is what the update left in the bones, and it
        // is handed to the device as rows: no cull runs here and no traversal number gates it.
        subject.mExtractor->extract(*subject.mNode, osg::Matrixf::identity(), 0, subject.mPosedFrame);

        // The sweep is what takes the parts that came off. It is sound for the same reason it is
        // sound for the world: this walk is the whole of what this picture is of.
        subject.mExtractor->retire();

        // It consumes the arrivals, so nothing here clears them. Never advanced, unlike the
        // world's frame: `Handing::mAdvance` says why a picture has no motion to describe.
        subject.mUploader.hand(mRenderer,
            SceneUploader::Handing{
                .mSlot = subject.mSlot.get(), .mScene = *subject.mScene, .mImages = images, .mAdvance = false });

        return subject.mScene->placements().getCounts().mPlaced > 0;
    }

    void OffscreenTrace::traceInto(const GuiSlot texture, const bool readBack)
    {
        const std::optional<Shaders::VisibilityConstants> camera = describeCamera();
        if (!camera.has_value())
            return;

        const osg::Vec4f& clear = mRequest.mClear;
        mRenderer.traceGuiTexture(texture, *camera,
            GuiTraceOptions{
                .mClear = { clear.r(), clear.g(), clear.b(), clear.a() },
                .mScene = mSubject != nullptr ? mSubject->mSlot.get() : SceneSlot::world(),
                .mReadBack = readBack,
            });
    }

    bool OffscreenTrace::takeCopy(const GuiSlot texture, const std::span<std::uint8_t> into) const
    {
        return mRenderer.takeGuiCopy(texture, into);
    }

    bool OffscreenTrace::pick(float x, float y, osg::NodePath& hit) const
    {
        if (mSubject == nullptr)
            return false;

        Subject& subject = *mSubject;
        const std::optional<Shaders::VisibilityConstants> camera = describeCamera();
        if (!camera.has_value())
            return false;

        const osg::Vec3f direction = camera->mCamera.mForward + camera->mCamera.mRight * x - camera->mCamera.mUp * y;

        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector = new osgUtil::LineSegmentIntersector(
            osgUtil::Intersector::MODEL, camera->mOrigin + direction * mRequest.mFraming.mNear,
            camera->mOrigin + direction * mRequest.mFraming.mFar);
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/sceneutil/offscreenframing.hpp>

#include "frameimage.hpp"
#include "renderer.hpp"
#include "sceneuploader.hpp"
#include "skylight.hpp"
#include "walk.hpp"

namespace osg
{
    class FrameStamp;
}

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class PoseCull;
    class PoseUpdate;
    class SceneDesc;
    class SceneExtractor;

    /// One picture traced from somewhere other than the eye: an inventory doll, a map tile. The
    /// trace writes straight into a slot of the renderer's GUI texture table, so the picture is
    /// never a framebuffer and never in main memory unless somebody asks `readGuiTexture`. Two
    /// kinds, and which constructor built it is which: a picture of the world traces against the
    /// scene the renderer already holds, and a picture of a subject is of a group assembled for it,
    /// mirrored into a scene of its own and walked again whenever the picture is asked for. The
    /// slot is handed to `traceInto` rather than owned here, so the harness can draw a doll with no
    /// GUI under it.
    class OffscreenTrace
    {
    public:
        /// A picture of the world the renderer already holds. Nothing is mirrored for it, so a
        /// picture taken before the first frame is a picture of nothing.
        ///
        /// @param rayMask which classes the picture's camera draws, `Shaders::MASK_*`. A map tile
        ///        leaves out the actors, the effects and the particles.
        OffscreenTrace(Renderer& renderer, std::uint32_t width, std::uint32_t height, std::uint32_t rayMask);

        /// A picture of a subtree assembled for it alone. A doll asks for every class of `rayMask`.
        ///
        /// @param mask which nodes the walk may descend into, AND-ed at every node.
        /// @param traversals where the walk's and the pick's traversal numbers come from, shared
        ///        with everything else that can reach the same nodes, because a subtree two walks
        ///        reach would otherwise be run by whichever got there first and frozen for the
        ///        other. Left out, this keeps a sequence of its own.
        OffscreenTrace(Renderer& renderer, std::uint32_t width, std::uint32_t height, std::uint32_t rayMask,
            osg::Node& subject, osg::Node::NodeMask mask, Traversals* traversals = nullptr);

        /// Out of line because `SceneDesc`, `SceneExtractor` and the update visitor are only forward
        /// declared here.
        ~OffscreenTrace();

        /// How the picture is projected and where it is clipped. Takes effect on the next trace.
        void setFraming(const SceneUtil::Framing& framing) { mFraming = framing; }

        /// The only light there is, in the numbers a rasterizer's object shaders were written for.
        /// A Lambertian surface returns `albedo / pi * E * cos`, so the `E` that makes that equal
        /// `albedo * diffuse` at `cos = 1` is `diffuse * pi`. `FlatLight::mDirection` is normalised
        /// here.
        void setLight(const SceneUtil::FlatLight& light);

        /// What the picture is left as where nothing was hit. An alpha below one is the whole of
        /// "the picture stops here".
        void setClearColour(const osg::Vec4f& colour);

        /// Which end of the picture the trace writes first — a delivery convention:
        /// `MWRender::OffscreenView::getTexture` promises rows bottom-first because that is what an
        /// OpenGL render-to-texture produces, and a file wants them the other way. Flipping the
        /// camera's up vector costs nothing.
        void setRowOrder(RowOrder order) { mRowOrder = order; }

        /// Where the picture is taken from. Takes effect on the next trace.
        void setView(const osg::Matrixf& view);

        /// Fill only this much of the picture, from its top-left corner, and leave the rest at the
        /// clear colour. Clamped to the size this was made at. For the inventory doll, whose window
        /// resizes while the texture behind it does not.
        void setExtent(std::uint32_t width, std::uint32_t height);

        bool isOfWorld() const { return mSubject == nullptr; }

        /// The mirror of the subject, or null for a picture of the world — which has no scene of its
        /// own, and traces against the one the frame's own walk built.
        const SceneDesc* getScene() const;

        /// Poses the subject, mirrors it and hands it to the renderer, and answers whether the
        /// result has anything in it. Nothing at all, and true, for a picture of the world.
        ///
        /// @param posing what the update traversal runs on — the caller's own drawing clock, because
        ///        a skeleton keeps the last number it saw and a clock that stood still would move the
        ///        doll's bones the first time and never again.
        /// @param worldFrame which of a `SceneUtil::LightSource`'s two buffers update just wrote,
        ///        which stops with the world when the game is paused; `posing` does not.
        bool rebuildSubject(const osg::FrameStamp& posing, std::size_t worldFrame, Resource::ImageManager& images);

        /// Traces the picture into `texture`, a slot from `Renderer::addGuiTexture`, and leaves a
        /// copy for `Renderer::takeGuiCopy` where `readBack` asks for one.
        void traceInto(GuiSlot texture, bool readBack = false);

        /// What is at this point of the picture, in normalised device coordinates, as the path
        /// through the subject to whatever was hit. Nothing for a picture of the world. On the
        /// processor and against the graph, because the caller wants a node path to ask the
        /// animation which equipment slot that was; the ray is the one the trace would have sent
        /// through that point. The one place a skinned body is still posed on the processor: the
        /// drawable's own copy holds the bind pose, so the subject is put through a cull of its own
        /// once per pick.
        bool pick(float x, float y, osg::NodePath& hit) const;

    private:
        /// The camera this picture is taken with, as the trace takes it. What `traceInto` traces
        /// with and what `pick` builds its ray from, so the two cannot disagree.
        Shaders::VisibilityConstants describeCamera() const;

        Renderer& mRenderer;

        /// Everything a picture of its own subject needs, and a picture of the world has none of:
        /// `mSubject` being null is the whole of what "this is a picture of the world" means.
        struct Subject
        {
            /// @param shared where the walk's and the pick's traversal numbers come from, or null to
            ///        keep a sequence of its own. Out of line with the destructor, because a
            ///        constructor that unwinds needs the forward-declared types complete too.
            explicit Subject(Traversals* shared);
            ~Subject();

            /// Not const, because a picture is taken by changing it: the update traversal poses
            /// the subject and the intersection visitor walks it, and both take a mutable node.
            osg::ref_ptr<osg::Node> mNode;

            /// The mirror of it, and the extractor that fills it.
            std::unique_ptr<SceneDesc> mScene;
            std::unique_ptr<SceneExtractor> mExtractor;

            /// Its own, because the only state one carries between calls is the clock it is
            /// given. The camera callback the game hangs on a doll's subtree is what finds the
            /// head to look at, and it runs in an update traversal — so a picture drawn between
            /// frames has to run one.
            std::unique_ptr<PoseUpdate> mUpdate;

            /// The cull traversal `pick` poses the subject with, and the clock it reads. Made once,
            /// because a cull carries a state graph and a render stage; the stamp is a copy of the
            /// last `rebuildSubject`'s, so a pick poses at the time the picture was taken.
            std::unique_ptr<PoseCull> mPose;
            osg::ref_ptr<osg::FrameStamp> mPoseStamp;

            /// Where the walk's and the pick's traversal numbers come from. `mOwn` is used only
            /// where the caller named none.
            Traversals mOwn;
            Traversals& mTraversals;

            /// A doll takes the same three branches a cell does, so a race-creation slider drag
            /// that redraws the same subject every frame is a placement rather than an acceleration
            /// structure and a texture array built from nothing sixty times a second.
            SceneUploader mUploader;

            /// The slot the renderer keeps this scene's acceleration structures in.
            SceneSlot mSlot;

            /// The traversal number the subject's update last ran at. What `rebuildSubject` hands
            /// the update traversal, and what a pick's own cull is dated after.
            unsigned int mPosedFrame = 0;
        };

        /// Null for a picture of the world, which traces against the scene the frame's own walk
        /// built.
        std::unique_ptr<Subject> mSubject;

        GuiTraceOptions mOptions;
        osg::Matrixf mView;

        /// The size the picture was made at, which is what `setExtent` is clamped against.
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        RowOrder mRowOrder = RowOrder::TopFirst;
        std::uint32_t mRayMask = 0;

        /// One value and not a flag beside four floats, three of which would mean nothing in
        /// whichever case the flag did not name, and all four of which the caller already holds as a
        /// `SceneUtil::Framing`.
        SceneUtil::Framing mFraming;

        /// Where the light stands, unit, in the sense `setLight` states it and the trace takes it.
        Sun mSun;
        osg::Vec3f mAmbient;

        bool mTransparent = false;
    };
}

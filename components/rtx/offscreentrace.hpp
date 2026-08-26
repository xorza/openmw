#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include "frameimage.hpp"
#include "renderer.hpp"
#include "sceneuploader.hpp"

namespace osg
{
    class FrameStamp;
}

namespace osgUtil
{
    class UpdateVisitor;
}

namespace Resource
{
    class ImageManager;
}

namespace Rtx
{
    class SceneDesc;
    class SceneExtractor;
    class Traversals;

    /// One picture traced from somewhere other than the eye: an inventory doll, a map tile.
    ///
    /// **Nothing is rendered to a texture and nothing is read back.** A rasterizer answers this with
    /// a pre-render camera hung off the graph and an `osg::Texture2D` under it; here the trace writes
    /// straight into a slot of the renderer's GUI texture table, so the picture is never a frame,
    /// never a framebuffer and never in main memory unless somebody asks `readGuiTexture` for it.
    ///
    /// **Two kinds, and which constructor built it is which.** A picture of the world traces against
    /// the scene the renderer already holds, so it owns no scene of its own and `rebuildSubject` has
    /// nothing to do. A picture of a subject is of a group somebody assembled for it — nothing in it
    /// stands in a cell — so it is mirrored into a scene of its own and walked again whenever the
    /// picture is asked for, which is when the character puts something on.
    ///
    /// **The trace and not the delivery.** What the picture is shown in — a `MyGUI::ITexture` in the
    /// game, a PNG in the harness — is the caller's, and the slot is handed to `traceInto` rather
    /// than owned here. That split is what lets the harness draw a doll with no GUI under it.
    class OffscreenTrace
    {
    public:
        /// A picture of the world the renderer already holds.
        ///
        /// Nothing is mirrored for it: the frame's own walk is what puts the geometry there, so a
        /// picture taken before the first frame is a picture of nothing and the caller is what has
        /// to wait.
        OffscreenTrace(Renderer& renderer, std::uint32_t width, std::uint32_t height);

        /// A picture of a subtree assembled for it alone.
        ///
        /// @param mask which nodes the walk may descend into. **An inclusion mask**, AND-ed at every
        ///        node, so a category left out of it is dropped wherever it appears below.
        /// @param traversals where the pose numbers come from. **Shared with everything else that
        ///        can reach the same nodes** — the game hands the same counter to the world's walk
        ///        and to every picture — because a subtree two walks reach would otherwise be posed
        ///        by whichever got there first and frozen for the other. Left out, this keeps a
        ///        sequence of its own, which is right where nothing else walks the subject.
        OffscreenTrace(Renderer& renderer, std::uint32_t width, std::uint32_t height, osg::Node& subject,
            osg::Node::NodeMask mask, Traversals* traversals = nullptr);

        /// Out of line because `SceneDesc`, `SceneExtractor` and the update visitor are only forward
        /// declared here.
        ~OffscreenTrace();

        OffscreenTrace(const OffscreenTrace&) = delete;
        OffscreenTrace& operator=(const OffscreenTrace&) = delete;

        /// A vertical field of view, in degrees.
        void setPerspective(float fieldOfView, float near, float far);

        /// A box this many world units across, centred on the view direction.
        void setOrthographic(float width, float height, float near, float far);

        /// The only light there is, in the numbers a rasterizer's object shaders were written for.
        ///
        /// **Converted here rather than by the caller**, because what a diffuse coefficient means as
        /// an irradiance is a fact about light transport and not about whoever asked for a picture.
        /// A Lambertian surface returns `albedo / pi * E * cos`, so the `E` that makes that equal
        /// `albedo * diffuse` at `cos = 1` is `diffuse * pi` — and with it a doll lit by the game's
        /// own numbers comes out the brightness those numbers were chosen for.
        ///
        /// @param towardsSun where the light comes from, normalised here.
        void setLight(const osg::Vec3f& towardsSun, const osg::Vec4f& diffuse, const osg::Vec4f& ambient);

        /// What the picture is left as where nothing was hit.
        ///
        /// **An alpha below one is the whole of "the picture stops here"**, because that is the same
        /// statement: one the GUI composites over what is behind it has to say where it ends, and
        /// one that fills its widget does not.
        void setClearColour(const osg::Vec4f& colour);

        /// Which end of the picture the trace writes first.
        ///
        /// **A delivery convention and not a fact about the picture**, which is why it is said here
        /// rather than assumed. `MWRender::OffscreenView::getTexture` promises rows bottom-first,
        /// because that is what an OpenGL render-to-texture produces and what the widgets showing
        /// one already invert V for; anything writing a file wants them the way round the trace
        /// makes them. Flipping the camera's up vector is what costs nothing and does it.
        void setRowOrder(RowOrder order) { mRowOrder = order; }

        /// Where the picture is taken from. Takes effect on the next trace.
        void setView(const osg::Matrixf& view);

        /// Fill only this much of the picture, from its top-left corner, and leave the rest at the
        /// clear colour. Clamped to the size this was made at.
        ///
        /// The inventory doll, whose window resizes while the texture behind it does not.
        void setExtent(std::uint32_t width, std::uint32_t height);

        bool isOfWorld() const { return mSubject == nullptr; }

        /// The mirror of the subject, or null for a picture of the world — which has no scene of its
        /// own, and traces against the one the frame's own walk built.
        const SceneDesc* getScene() const { return mScene.get(); }

        /// Poses the subject, mirrors it and hands it to the renderer, and answers whether the
        /// result has anything in it. Nothing at all, and true, for a picture of the world.
        ///
        /// **Two clocks, because they are two different questions.**
        ///
        /// @param posing what the update traversal runs on, and where the pose number comes from.
        ///        A picture is redrawn when its subject changes rather than when the world moves, so
        ///        this is the caller's own drawing clock — one that stands still would skin the doll
        ///        the first time and never again.
        /// @param worldFrame which of a `SceneUtil::LightSource`'s two buffers update has just
        ///        written, which is a property of the frame the *world* is in. It stops with the
        ///        world when the game is paused; `posing` does not.
        bool rebuildSubject(const osg::FrameStamp& posing, std::size_t worldFrame, Resource::ImageManager& images);

        /// Traces the picture into `texture`, a slot from `Renderer::addGuiTexture`.
        void traceInto(std::uint32_t texture);

        /// What is at this point of the picture, in normalised device coordinates, as the path
        /// through the subject to whatever was hit. Nothing for a picture of the world, which is
        /// asked what is where by whoever placed it rather than by a ray.
        ///
        /// **On the processor and against the graph, not on the device.** What the caller wants back
        /// is a node path, so it can ask the animation which equipment slot that was; a ray query
        /// gives an instance index in a mirror, which is the wrong side of the question. The ray is
        /// the one the trace would have sent through that point, built from the same basis, so what
        /// a click finds is what the picture shows.
        bool pick(float x, float y, osg::NodePath& hit) const;

    private:
        /// The camera this picture is taken with, as the trace takes it. What `traceInto` traces
        /// with and what `pick` builds its ray from, so the two cannot disagree.
        Shaders::VisibilityConstants describeCamera() const;

        Renderer& mRenderer;

        /// What this is a picture of, where it is not the world. Held because it is walked again
        /// every time the picture is asked for.
        ///
        /// **Not const, because a picture is taken by changing it**: the update traversal poses the
        /// subject and the intersection visitor walks it, and both take a mutable node.
        osg::ref_ptr<osg::Node> mSubject;

        /// The mirror of it, and the slot the renderer keeps its acceleration structures in. Both
        /// null and unused for a picture of the world.
        std::unique_ptr<SceneDesc> mScene;
        std::unique_ptr<SceneExtractor> mExtractor;

        /// **Its own, because the only state one carries between calls is the clock it is given.**
        /// The camera callback the game hangs on a doll's subtree is what finds the head to look at,
        /// and it runs in an update traversal — so a picture drawn between frames has to run one.
        std::unique_ptr<osgUtil::UpdateVisitor> mUpdate;

        /// **A doll takes the same three branches a cell does.** A race-creation slider drag redraws
        /// the same subject every frame, and this is what makes that a placement rather than an
        /// acceleration structure and a texture array built from nothing sixty times a second.
        SceneUploader mUploader;

        std::uint32_t mViewScene = 0;

        /// The traversal number the subject was last posed at. What an intersection test has to be
        /// told, because a skinned mesh keeps two poses and picks between them by frame.
        unsigned int mPosedFrame = 0;

        GuiTraceOptions mOptions;
        osg::Matrixf mView;

        /// The size the picture was made at, which is what `setExtent` is clamped against.
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;

        RowOrder mRowOrder = RowOrder::TopFirst;

        bool mPerspective = true;
        float mFieldOfView = 0.f;
        float mBoxWidth = 0.f;
        float mBoxHeight = 0.f;
        float mNear = 1.f;
        float mFar = 10000.f;

        /// Where the light stands, unit — which is what the trace takes, and so already the sense
        /// `setLight` states it in.
        osg::Vec3f mSunPosition;
        osg::Vec3f mSunIrradiance;
        osg::Vec3f mAmbient;

        bool mTransparent = false;
    };
}

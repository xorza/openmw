#ifndef GAME_RENDER_STAGE_H
#define GAME_RENDER_STAGE_H

#include <osg/ref_ptr>

namespace osg
{
    class Camera;
    class FrameStamp;
    class Group;
    class Stats;
}

namespace osgGA
{
    class EventQueue;
}

namespace osgUtil
{
    class UpdateVisitor;
}

namespace MWRender
{
    /// The frame, the eye and the input queue — where the game reads them, whatever draws.
    ///
    /// **`osgViewer::Viewer` was two things and thirteen classes wanted the smaller one.** It
    /// bundles the frame stamp, the master camera, the event queue and the update traversal — none
    /// of which touches OpenGL — with a graphics context, a threading model and a draw dispatcher.
    /// Everything from the GUI to the input wrapper had to import `osgViewer` to reach the first
    /// half, and that is a good part of why the renderer looked unswappable. `Stage` is the first
    /// half, named; `MWRender::Renderer` is the second.
    ///
    /// **Filled in by the renderer rather than filled in for it.** Every renderer needs a camera, a
    /// frame stamp, an input queue and somewhere to count things, and one built on `osgViewer` gets
    /// all four already wired to each other — the update and event visitors hold the frame stamp,
    /// and swapping the objects underneath without swapping those references is a bug that only
    /// shows up frames later. So the renderer says which objects it drives the frame from and the
    /// stage remembers them, which costs a renderer that owns its own surface one constructor call
    /// and costs this one nothing at all.
    class Stage
    {
    public:
        Stage();
        ~Stage();

        Stage(const Stage&) = delete;
        Stage& operator=(const Stage&) = delete;

        /// Called once, by the renderer being constructed, before anything above it exists.
        void adopt(osg::Camera& camera, osg::FrameStamp& frameStamp, osgGA::EventQueue& events,
            osgUtil::UpdateVisitor& updateVisitor, osg::Stats& stats);

        /// View, projection, viewport and cull mask. Whether there is a graphics context behind it
        /// is the renderer's business, and under one that owns its own surface there is none.
        osg::Camera& getCamera() const;

        /// Frame number, simulation time and reference time, advanced once per frame.
        osg::FrameStamp& getFrameStamp() const;

        /// Where SDL puts what it read, and where the scene graph's own handlers read it from.
        osgGA::EventQueue& getEvents() const;

        osgUtil::UpdateVisitor& getUpdateVisitor() const;

        /// Per-frame counters, keyed by frame number. Every subsystem reports into this one.
        osg::Stats& getStats() const;

        /// Whatever is topmost. Not always the node the world was built under: the rasterizer wraps
        /// it in its post-processing group, and the render-to-texture cameras the GUI hangs off the
        /// top have to land above that too.
        osg::Group& getSceneRoot() const;

        /// Also parents `root` under the camera, which is what makes the world reachable to an
        /// intersection visitor and so to everything that asks what the player is looking at.
        ///
        /// Set through `Renderer::setSceneRoot`, so that the renderer and the stage cannot disagree
        /// about what is topmost.
        void setSceneRoot(osg::Group& root);

    private:
        osg::ref_ptr<osg::Camera> mCamera;
        osg::ref_ptr<osg::FrameStamp> mFrameStamp;
        osg::ref_ptr<osgGA::EventQueue> mEvents;
        osg::ref_ptr<osgUtil::UpdateVisitor> mUpdateVisitor;
        osg::ref_ptr<osg::Stats> mStats;
        osg::ref_ptr<osg::Group> mSceneRoot;
    };
}

#endif

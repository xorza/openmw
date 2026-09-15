#ifndef GAME_RENDER_OFFSCREENVIEW_H
#define GAME_RENDER_OFFSCREENVIEW_H

#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/sceneutil/offscreenframing.hpp>

namespace MyGUI
{
    class ITexture;
}

namespace osg
{
    class Image;
}

namespace MWRender
{

    /// A picture taken somewhere other than the eye, described in numbers the game already has:
    /// sample counts, formats, cull modes and blend functions are the renderer's. Every field but
    /// the reference carries its own initializer, because a caller that names some of them reads
    /// to GCC as a short aggregate otherwise. Which of the two kinds of picture it describes is
    /// said by the call that takes it — `Renderer::createWorldView` or `createSubjectView`.
    struct OffscreenViewSpec
    {
        /// The subtree to draw: the world's own scene root for a world view, which arrives already
        /// lit, already placed relative to the eye and already brought up to date this frame; a
        /// group the game assembled for this picture alone for a subject view, which has none of
        /// that and is given all of it. Every `redraw()` updates it and then draws it, so an update
        /// callback here is where a view that follows something inside it works out where to look.
        osg::Node& mScene;

        /// The image, in pixels. `setExtent` may go on to fill less of it than this.
        int mWidth = 0;
        int mHeight = 0;

        /// Only the nodes these bits select (`MWRender::VisMask`): an inclusion mask AND-ed at
        /// every node, so a category left out is dropped wherever it appears below. The rasterizer
        /// puts it on the camera's cull mask; a ray tracer masks the walk of a subject with it and
        /// hands every picture the classes its rays meet (`rayMaskOf`).
        unsigned int mMask = ~0u;

        SceneUtil::Framing mFraming{};

        /// Behind everything, and seen through whatever the picture does not cover: the GUI
        /// composites the result rather than filling a widget with it.
        osg::Vec4f mClearColour{};

        /// The only light there is, pointing towards where it comes from.
        SceneUtil::FlatLight mSun{};
    };

    /// One such picture, alive for as long as the GUI shows it: what every kind of picture has —
    /// a viewpoint, a redraw, a copy and a texture. A tile of the world is one of these and
    /// nothing more.
    class OffscreenView
    {
    public:
        virtual ~OffscreenView() = default;

        OffscreenView(const OffscreenView&) = delete;
        OffscreenView& operator=(const OffscreenView&) = delete;

        /// Where the picture is taken from. Takes effect on the next `redraw()`.
        virtual void setView(const osg::Matrixf& view) = 0;

        /// Update the subtree and draw it again. Not per frame: a doll is redrawn when the player
        /// puts something on, and a map tile when its cell is first entered.
        virtual void redraw() = 0;

        /// Also keep the picture in main memory from now on. Costs a transfer off the device every
        /// time it is drawn, so it is asked for rather than always done. A view whose last picture
        /// was drawn without the copy draws once more, so that asking and then reading `getCopy`
        /// every frame is all a caller has to do; asking again is free.
        virtual void keepCopy() = 0;

        /// That copy, or null while the most recent `redraw()` has not reached it, which is frames
        /// later; null forever where nothing asked for one. Not const, because the tracer takes the
        /// copy off the device the first time it is asked for after it has arrived.
        virtual const osg::Image* getCopy() = 0;

        /// What the GUI shows, Y-up, so the widget showing it inverts V; a renderer that writes the
        /// other way round owes the flip, or every caller asks which renderer it got.
        virtual MyGUI::ITexture& getTexture() const = 0;

    protected:
        OffscreenView() = default;
    };

    /// A picture of a subject the game assembled for it — the inventory doll, the race preview —
    /// which is also resized under its window, rebuilt when the subject is dressed, and picked at.
    class SubjectView : public OffscreenView
    {
    public:
        /// Fill only this much of the image and leave the rest at the clear colour: the inventory
        /// doll, whose window resizes while the texture behind it does not. A description like
        /// `setView`, which the next `redraw()` acts on, so both renderers repaint at the same call.
        virtual void setExtent(int width, int height) = 0;

        /// The subtree is not the same subtree any more — geometry added, removed or replaced,
        /// rather than moved. What that costs is the renderer's business; a pose change is not it.
        virtual void sceneChanged() = 0;

        /// What is at this point of the picture, in normalised device coordinates, as the path
        /// through the subtree to whatever was hit — against the drawn picture, because skinned
        /// geometry is double-buffered by frame number.
        virtual bool pick(float x, float y, osg::NodePath& hit) const = 0;

    protected:
        SubjectView() = default;
    };

}

#endif

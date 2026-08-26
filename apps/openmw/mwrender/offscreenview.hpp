#ifndef GAME_RENDER_OFFSCREENVIEW_H
#define GAME_RENDER_OFFSCREENVIEW_H

#include <variant>

#include <osg/Matrixf>
#include <osg/Node>
#include <osg/Vec3f>
#include <osg/Vec4f>

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

    /// A picture of part of the world taken somewhere other than the eye, described in numbers the
    /// game already has.
    ///
    /// **Nothing here is an OpenGL decision.** Sample counts, colour formats, depth conventions,
    /// render order, cull modes and blend functions are the renderer's, worked out from what was
    /// asked for rather than passed down through it. A ray tracer answers the same request with
    /// rays, and none of those words mean anything to it.
    ///
    struct OffscreenViewSpec
    {
        /// A vertical field of view, in degrees.
        struct Perspective
        {
            float mFieldOfView = 0.f;
        };

        /// A box this many world units across, centred on the view direction.
        struct Orthographic
        {
            float mWidth = 0.f;
            float mHeight = 0.f;
        };

        /// The subtree to draw. Every `redraw()` updates it and then draws it, so an update
        /// callback here is where a view that follows something inside it works out where to look
        /// from — the only moment at which it can.
        osg::Node& mScene;

        /// The image, in pixels. `setExtent` may go on to fill less of it than this.
        int mWidth = 0;
        int mHeight = 0;

        /// Only the nodes these bits select; `vismask.hpp`.
        ///
        /// **An inclusion mask, AND-ed at every node**, so a category left out of it is dropped
        /// wherever it appears below — which is a shape worth naming, because the same shape once
        /// left the weather's particles out of every storm.
        ///
        /// **And the two renderers read it in different places.** The rasterizer puts it on the
        /// camera's cull mask, so it applies to every picture. A ray tracer has no cull: it applies
        /// only where `mFromWorld` is false and there is a walk of this subtree to mask, because a
        /// picture of the world traces against the scene the frame's own walk already built and is
        /// masked by whatever masked that. A map tile is therefore drawn with everything the frame
        /// holds, this field notwithstanding.
        unsigned int mMask = ~0u;

        std::variant<Perspective, Orthographic> mProjection;
        float mNear = 1.f;
        float mFar = 10000.f;

        /// Behind everything, and seen through whatever the picture does not cover: the GUI
        /// composites the result rather than filling a widget with it.
        osg::Vec4f mClearColour;

        /// The only light there is, pointing towards where it comes from.
        osg::Vec3f mSunDirection;
        osg::Vec4f mSunDiffuse;
        osg::Vec4f mSunAmbient;

        /// Whether `mScene` is a piece of the world or a group the game assembled for this picture
        /// alone. The world arrives already lit, already placed relative to the eye and already
        /// brought up to date this frame; a bare subtree has none of that and is given all of it
        /// here.
        bool mFromWorld = false;
    };

    /// One such picture, alive for as long as the GUI shows it.
    class OffscreenView
    {
    public:
        virtual ~OffscreenView() = default;

        OffscreenView(const OffscreenView&) = delete;
        OffscreenView& operator=(const OffscreenView&) = delete;

        /// Where the picture is taken from. Takes effect on the next `redraw()`.
        virtual void setView(const osg::Matrixf& view) = 0;

        /// Fill only this much of the image and leave the rest at the clear colour. The inventory
        /// doll, whose window resizes while the texture behind it does not.
        ///
        /// **A description and not a command**, like `setView`: it says what the next `redraw()`
        /// should fill and does not itself draw. One renderer used to redraw here and the other did
        /// not, so a resize repainted the doll under the rasterizer and left it stale under the ray
        /// tracer — the kind of difference a caller cannot see and would have had to ask which
        /// renderer it got to predict.
        virtual void setExtent(int width, int height) = 0;

        /// The subtree is not the same subtree any more — geometry added, removed or replaced,
        /// rather than moved. What that costs is the renderer's business; a pose change is not it.
        virtual void sceneChanged() = 0;

        /// Update the subtree and draw it again. Not per frame: a doll is redrawn when the player
        /// puts something on, and a map tile when its cell is first entered.
        virtual void redraw() = 0;

        /// Also keep the picture in main memory from now on. Costs a transfer off the device every
        /// time it is drawn, so it is asked for rather than always done.
        virtual void keepCopy() = 0;

        /// That copy, or null while the most recent `redraw()` has not reached it — the drawing has
        /// not happened when `redraw()` returns, and the copy comes back after that again. Null
        /// forever where nothing asked for one.
        virtual const osg::Image* getCopy() const = 0;

        /// What is at this point of the picture, in normalised device coordinates, as the path
        /// through the subtree to whatever was hit.
        ///
        /// **Against the drawn picture and not the current one.** Skinned geometry is
        /// double-buffered by frame number, so a query that means "what did I click on" has to name
        /// the frame that was drawn rather than the frame being built.
        virtual bool pick(float x, float y, osg::NodePath& hit) const = 0;

        /// What the GUI shows. **Y-up**, so the widget showing it inverts V; a renderer that would
        /// rather write the other way round still owes this one, because the alternative is every
        /// caller asking which renderer it got.
        virtual MyGUI::ITexture& getTexture() const = 0;

    protected:
        OffscreenView() = default;
    };

}

#endif

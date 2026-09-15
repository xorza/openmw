#ifndef GAME_RENDER_GLOFFSCREENVIEW_H
#define GAME_RENDER_GLOFFSCREENVIEW_H

#include <memory>

#include <osg/ref_ptr>

#include "offscreenview.hpp"

namespace MyGUIPlatform
{
    class OSGTexture;
}

namespace osg
{
    class FrameStamp;
    class Group;
    class Image;
    class StateSet;
}

namespace Resource
{
    class ResourceSystem;
}

namespace SceneUtil
{
    class RTTNode;
}

namespace MWRender
{
    class CharacterPreviewRTTNode;
    class DrawOnceCallback;
    class LocalMapRenderToTexture;

    /// An offscreen view as OpenSceneGraph draws one: a pre-render camera hung off the top of the
    /// graph, culled and drawn by the same traversal as everything else.
    ///
    /// The glue every such picture shares — the GUI texture, the copy in main memory, the pick —
    /// over whichever of upstream's two render-to-texture nodes a subclass owns. The nodes are
    /// upstream's own, moved from `characterpreview.cpp` and `localmap.cpp`, so what a doll or a
    /// map tile looks like is what it looked like before there was a second renderer.
    class GlOffscreenView : public OffscreenView
    {
    public:
        ~GlOffscreenView() override;

        void keepCopy() override;
        const osg::Image* getCopy() override;
        bool pick(float x, float y, osg::NodePath& hit) const override;
        MyGUI::ITexture& getTexture() const override;

    protected:
        /// @param node the subclass's, parented under `parent` here and unparented in the destructor
        /// @param blend what the widget draws the picture with, or null for the widget's own
        GlOffscreenView(
            SceneUtil::RTTNode& node, osg::Group& parent, const osg::FrameStamp& frameStamp, osg::StateSet* blend);

        /// The traversal number of the last draw, nought before the first.
        virtual unsigned int getDrawnFrame() const = 0;

        /// Whether a draw has been asked for and not yet made.
        virtual bool isPending() const = 0;

    private:
        osg::ref_ptr<osg::Group> mParent;
        osg::ref_ptr<SceneUtil::RTTNode> mNode;
        const osg::FrameStamp& mFrameStamp;
        osg::ref_ptr<osg::Image> mCopy;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mTexture;
    };

    /// A picture of a subtree the game assembled for it alone — the inventory doll, the race
    /// preview — drawn by upstream's `CharacterPreviewRTTNode` under the light rig, the sample count
    /// and the premultiplied alpha that `CharacterPreview` used to set up for itself.
    class GlDollView final : public GlOffscreenView
    {
    public:
        GlDollView(const OffscreenViewSpec& spec, osg::Group& parent, const osg::FrameStamp& frameStamp,
            Resource::ResourceSystem& resources);
        ~GlDollView() override;

        void setView(const osg::Matrixf& view) override;
        void setExtent(int width, int height) override;
        void sceneChanged() override;
        void redraw() override;

    protected:
        unsigned int getDrawnFrame() const override;
        bool isPending() const override;

    private:
        GlDollView(const OffscreenViewSpec& spec, CharacterPreviewRTTNode& node, osg::Group& parent,
            const osg::FrameStamp& frameStamp, Resource::ResourceSystem& resources);

        /// The base owns the node; this is the same node by its own type.
        CharacterPreviewRTTNode& mDoll;
        osg::ref_ptr<DrawOnceCallback> mDrawOnce;
        osg::ref_ptr<osg::Node> mScene;
    };

    /// A tile of the world seen straight down, drawn by upstream's `LocalMapRenderToTexture` with
    /// the frame's own light manager under it and the sun overridden, as the local map always was.
    class GlTileView final : public GlOffscreenView
    {
    public:
        GlTileView(const OffscreenViewSpec& spec, osg::Group& parent, const osg::FrameStamp& frameStamp);

        void setView(const osg::Matrixf& view) override;

        /// A tile is drawn whole; asking for less is a caller's mistake.
        void setExtent(int width, int height) override;

        /// Nothing in a tile is composited over what is behind it.
        void sceneChanged() override {}

        void redraw() override;

    protected:
        unsigned int getDrawnFrame() const override;
        bool isPending() const override;

    private:
        GlTileView(LocalMapRenderToTexture& node, osg::Group& parent, const osg::FrameStamp& frameStamp);

        /// The base owns the node; this is the same node by its own type.
        LocalMapRenderToTexture& mTile;
    };
}

#endif

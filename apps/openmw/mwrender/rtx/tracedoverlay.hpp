#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <osg/ref_ptr>

#include <components/sceneutil/imageregion.hpp>

#include "../mapoverlay.hpp"

namespace MyGUI
{
    class ITexture;
}

namespace osg
{
    class Image;
}

namespace SceneUtil
{
    class PaintedTexture;
}

namespace MyGUIRtx
{
    class RenderManager;
}

namespace MWRender
{
    class RtxRenderer;

    /// The world map's overlay as the ray tracer draws it: composited in main memory, in the image
    /// the save is written from, and mirrored into the interface through a `PaintedTexture` that
    /// says which cell was painted. `sampleBilinear` is what a `GL_LINEAR` sampler does, so the map
    /// is the map the rasterizer's camera composites.
    ///
    /// **A tile's picture comes back off the device frames after it was traced**, so a paint is
    /// kept until the copy is there and finished then — `finish`, once a frame from the renderer,
    /// after the frame behind has been collected — and never polled for from the interface.
    class TracedOverlay final : public MapOverlay
    {
    public:
        TracedOverlay(const MapOverlaySpec& spec, RtxRenderer& renderer, MyGUIRtx::RenderManager& gui);
        ~TracedOverlay() override;

        void paintTile(const SceneUtil::ImageRegion& destination, std::shared_ptr<OffscreenView> tile) override;
        void paintImage(const SceneUtil::ImageRegion& destination, osg::ref_ptr<osg::Image> image,
            const SceneUtil::ImageRegion& source) override;
        void replace(osg::ref_ptr<osg::Image> image) override;
        void clear() override;
        const osg::Image& getImage() const override { return *mImage; }
        MyGUI::ITexture& getTexture() override { return *mGuiTexture; }

        /*internal:*/
        /// Paints every tile whose picture has arrived since the last call. From the renderer,
        /// once a frame, where the copies of the frame behind are known to be complete.
        void finish();

    private:
        /// A cell asked for and not yet painted, because its picture has not come back.
        struct Pending
        {
            SceneUtil::ImageRegion mDestination;
            std::shared_ptr<OffscreenView> mTile;
        };

        /// `compositeTile` into the image, and the texture told where it changed.
        void composite(const SceneUtil::ImageRegion& destination, const osg::Image& tile);

        RtxRenderer& mRenderer;

        int mWidth;
        int mHeight;

        /// Where the land is above water: what stops an explored tile painting its cell's sea over
        /// the map's own.
        osg::ref_ptr<osg::Image> mLandAlpha;

        /// The overlay as drawn, told which cell was painted into the image below.
        osg::ref_ptr<SceneUtil::PaintedTexture> mTexture;

        /// The overlay in main memory: the texture's own image, whose identity never changes so
        /// what is drawn from it keeps drawing.
        osg::ref_ptr<osg::Image> mImage;

        /// One cell's worth of composited pixels, kept so painting one allocates nothing.
        std::vector<std::uint8_t> mCellScratch;

        /// Tiles whose picture has not come back yet, in the order asked. A second ask for a
        /// rectangle replaces the first.
        std::vector<Pending> mPending;

        std::unique_ptr<MyGUI::ITexture> mGuiTexture;
    };
}

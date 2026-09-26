#pragma once

#include <memory>

#include <osg/ref_ptr>

#include <components/sceneutil/imageregion.hpp>

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
    class OffscreenView;

    /// What the world map's overlay is built from.
    struct MapOverlaySpec
    {
        int mWidth = 0;
        int mHeight = 0;

        /// Where land stands above the sea, one alpha byte a texel over the whole map: an explored
        /// tile paints nothing below the water line.
        osg::ref_ptr<osg::Image> mLandAlpha;
    };

    /// The explored cells painted over the world map's base: the texture the GUI shows, and the
    /// same pixels in main memory for the save. A picture the renderer makes, as `OffscreenView`
    /// is, because how a tile gets into it is the renderer's — the rasterizer blits it with a camera
    /// and the ray tracer composites it in main memory — and `GlobalMap` asks the same of both.
    /// Every rectangle counts rows from the bottom, as `osg::Image` does.
    class MapOverlay
    {
    public:
        virtual ~MapOverlay() = default;

        MapOverlay(const MapOverlay&) = delete;
        MapOverlay& operator=(const MapOverlay&) = delete;

        /// Paints the whole of `tile`'s picture into `destination`, through the land alpha. The
        /// tile's picture may arrive frames after it was asked for, and the overlay finishes the
        /// paint itself; a second paint of the same rectangle before then replaces the first.
        virtual void paintTile(const SceneUtil::ImageRegion& destination, std::shared_ptr<OffscreenView> tile) = 0;

        /// Clears the overlay and paints `source` of `image` into `destination`, filtered: a save
        /// written at another map size or over another region of cells.
        virtual void paintImage(const SceneUtil::ImageRegion& destination, osg::ref_ptr<osg::Image> image,
            const SceneUtil::ImageRegion& source)
            = 0;

        /// Every pixel becomes `image`'s, which is the overlay's own size and format: a save written
        /// at this map's size.
        virtual void replace(osg::ref_ptr<osg::Image> image) = 0;

        virtual void clear() = 0;

        /// The overlay as it stands in main memory, for the save and the PNG. What a paint put into
        /// the texture reaches this a frame or two later under the rasterizer, as it always did.
        virtual const osg::Image& getImage() const = 0;

        virtual MyGUI::ITexture& getTexture() = 0;

    protected:
        MapOverlay() = default;
    };
}

#ifndef GAME_RENDER_GLOBALMAP_H
#define GAME_RENDER_GLOBALMAP_H

#include <cstdint>
#include <vector>

#include <osg/ref_ptr>

#include <components/myguiplatform/picture.hpp>

namespace MyGUI
{
    class ITexture;
}

namespace osg
{
    class Image;
}

namespace ESM
{
    struct GlobalMap;
}

namespace SceneUtil
{
    class WorkQueue;
}

namespace MWRender
{

    class CreateMapWorkItem;

    /// The world map: the land painted from its own heightmap, and over it the pieces of it the
    /// player has walked.
    ///
    /// **All of it is in main memory and none of it is rendered.** The overlay is composed out of
    /// local map tiles, which is what it always was; the render-to-texture it used to go through
    /// existed to do one downscale on the device, and paid for that with a camera per cell, a
    /// shader, a second copy of the image and a read back to keep the two in step. A box filter over
    /// the tile is both cheaper and better — the device sampled four texels of a picture it was
    /// shrinking fourteen-fold.
    class GlobalMap
    {
    public:
        explicit GlobalMap(SceneUtil::WorkQueue* workQueue);
        ~GlobalMap();

        GlobalMap(const GlobalMap&) = delete;
        GlobalMap& operator=(const GlobalMap&) = delete;

        void render();

        int getWidth() const { return mWidth; }
        int getHeight() const { return mHeight; }

        void worldPosToImageSpace(float x, float z, float& imageX, float& imageY);

        /// Paint a cell the player has walked into the overlay.
        ///
        /// @param tile the local map's picture of that cell, RGBA and one byte a channel, or null
        ///        while it has not been drawn yet.
        /// @return whether it was painted. A caller handed nothing asks again later.
        bool exploreCell(int cellX, int cellY, const osg::Image* tile);

        /// Clears the overlay
        void clear();

        void write(ESM::GlobalMap& map);
        void read(ESM::GlobalMap& map);

        MyGUI::ITexture& getBaseTexture();
        MyGUI::ITexture& getOverlayTexture();

        void ensureLoaded();

        void asyncWritePng();

    private:
        struct WritePng;

        osg::ref_ptr<SceneUtil::WorkQueue> mWorkQueue;
        osg::ref_ptr<CreateMapWorkItem> mWorkItem;
        osg::ref_ptr<WritePng> mWritePng;

        /// Where the land is above water. What stops an explored tile from painting its cell's sea
        /// over the map's own; the land itself goes straight into a texture and is not kept.
        osg::ref_ptr<osg::Image> mAlphaImage;

        /// What the player has walked, and the only copy of it: this is what is saved.
        osg::ref_ptr<osg::Image> mOverlayImage;

        /// The two pictures the GUI shows, which is what `MyGUIPlatform::Picture` is for: making
        /// the texture, keeping it while its shape holds, and writing part of it where the backend
        /// can take part of one.
        MyGUIPlatform::Picture mBase{ "global map" };
        MyGUIPlatform::Picture mOverlay{ "global map overlay" };

        /// One cell's worth of composited pixels, kept so that painting one allocates nothing and
        /// so that a repaint that changes nothing can be recognised before the upload.
        std::vector<std::uint8_t> mCellScratch;

        int mWidth = 0;
        int mHeight = 0;

        int mMinX = 0, mMaxX = 0, mMinY = 0, mMaxY = 0;
    };

}

#endif

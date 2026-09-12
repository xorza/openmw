#ifndef GAME_RENDER_GLOBALMAP_H
#define GAME_RENDER_GLOBALMAP_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <osg/ref_ptr>

namespace osg
{
    class Texture2D;
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
    /// player has walked, composited in main memory. `sampleBilinear` is what a sampler set to
    /// `GL_LINEAR` does, so the map is the map the render-to-texture used to draw.
    class GlobalMap
    {
    public:
        GlobalMap(SceneUtil::WorkQueue* workQueue);
        ~GlobalMap();

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

        osg::ref_ptr<osg::Texture2D> getBaseTexture();
        osg::ref_ptr<osg::Texture2D> getOverlayTexture();

        void ensureLoaded();

        void asyncWritePng();

    private:
        struct WritePng;

        osg::ref_ptr<osg::Texture2D> mBaseTexture;

        // Where the land is above water: what stops an explored tile painting its cell's sea over the map's own
        osg::ref_ptr<osg::Image> mAlphaImage;

        // GPU copy of overlay, drawn from the image below; osg::Image::dirty() is what sends a change up
        osg::ref_ptr<osg::Texture2D> mOverlayTexture;

        // CPU copy of overlay
        osg::ref_ptr<osg::Image> mOverlayImage;

        // One cell's worth of composited pixels, kept so painting one allocates nothing
        std::vector<std::uint8_t> mCellScratch;

        osg::ref_ptr<SceneUtil::WorkQueue> mWorkQueue;
        osg::ref_ptr<CreateMapWorkItem> mWorkItem;
        osg::ref_ptr<WritePng> mWritePng;

        int mWidth;
        int mHeight;

        int mMinX, mMaxX, mMinY, mMaxY;
    };

}

#endif

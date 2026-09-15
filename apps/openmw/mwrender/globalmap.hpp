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

    /// The world map, composited in main memory. sampleBilinear is what a GL_LINEAR sampler does, so the map is the
    /// map the render-to-texture used to draw.
    class GlobalMap
    {
    public:
        GlobalMap(SceneUtil::WorkQueue* workQueue);
        ~GlobalMap();

        void render();

        int getWidth() const { return mWidth; }
        int getHeight() const { return mHeight; }

        void worldPosToImageSpace(float x, float z, float& imageX, float& imageY);

        /// Paints the local map's picture of a cell (RGBA, one byte a channel) into the overlay.
        /// @return whether it was painted; a null tile is one not drawn yet, and the caller asks again later.
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

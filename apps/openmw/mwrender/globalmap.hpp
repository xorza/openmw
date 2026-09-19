#ifndef GAME_RENDER_GLOBALMAP_H
#define GAME_RENDER_GLOBALMAP_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <osg/ref_ptr>

namespace MyGUI
{
    class ITexture;
}

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
    class MapOverlay;
    class OffscreenView;
    class Renderer;

    class GlobalMap
    {
    public:
        /// @param renderer which makes the overlay the explored cells are painted into
        GlobalMap(Renderer& renderer, SceneUtil::WorkQueue* workQueue);
        ~GlobalMap();

        void render();

        int getWidth() const { return mWidth; }
        int getHeight() const { return mHeight; }

        void worldPosToImageSpace(float x, float z, float& imageX, float& imageY);

        /// Paints the local map's picture of a cell into the overlay. A null tile is a cell the
        /// local map has not been asked for, and paints nothing.
        void exploreCell(int cellX, int cellY, std::shared_ptr<OffscreenView> tile);

        /// Clears the overlay
        void clear();

        void write(ESM::GlobalMap& map);
        void read(ESM::GlobalMap& map);

        osg::ref_ptr<osg::Texture2D> getBaseTexture();
        MyGUI::ITexture& getOverlayTexture();

        void ensureLoaded();

        void asyncWritePng();

    private:
        struct WritePng;

        Renderer& mRenderer;

        osg::ref_ptr<osg::Texture2D> mBaseTexture;

        // The explored cells over the base, as whichever renderer draws paints them
        std::unique_ptr<MapOverlay> mOverlay;

        osg::ref_ptr<SceneUtil::WorkQueue> mWorkQueue;
        osg::ref_ptr<CreateMapWorkItem> mWorkItem;
        osg::ref_ptr<WritePng> mWritePng;

        int mWidth;
        int mHeight;

        int mMinX, mMaxX, mMinY, mMaxY;
    };

}

#endif

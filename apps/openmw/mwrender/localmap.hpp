#ifndef GAME_RENDER_LOCALMAP_H
#define GAME_RENDER_LOCALMAP_H

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <MyGUI_Types.h>
#include <osg/BoundingBox>
#include <osg/Quat>
#include <osg/ref_ptr>

#include <components/myguiplatform/picture.hpp>

namespace MWWorld
{
    class CellStore;
}

namespace MyGUI
{
    class ITexture;
}

namespace ESM
{
    struct FogTexture;
}

namespace osg
{
    class Group;
    class Image;
    class Node;
}

namespace MWRender
{
    class OffscreenView;
    class Renderer;

    ///
    /// \brief Local map rendering
    ///
    class LocalMap
    {
    public:
        /// @param root where the world was built; the map is a picture of what is named "Scene Root"
        ///        inside it, which is the whole of what the map has to do with the graph.
        LocalMap(Renderer& renderer, osg::Group& root);
        ~LocalMap();

        /**
         * Clear all savegame-specific data (i.e. fog of war textures)
         */
        void clear();

        /**
         * Request a map render for the given cell. Render textures will be immediately created and can be retrieved
         * with the getMapTexture function.
         */
        void requestMap(const MWWorld::CellStore* cell);

        void addCell(MWWorld::CellStore* cell);
        void removeExteriorCell(int x, int y);

        void removeCell(MWWorld::CellStore* cell);

        /// The picture of this segment, for a widget to show. Null where the cell has not been
        /// mapped.
        MyGUI::ITexture* getMapTexture(int x, int y);

        /// The same picture in main memory, for the global map to composite into its overlay, or
        /// null while the render has not come back off the device yet — ask again next frame.
        ///
        /// **Asking is what starts it, which is why this is not `const`.** A copy costs a read back
        /// off the device, and the world map wants one for the cell the player walked into: one of
        /// the nine an arrival draws. Keeping one for every tile drawn spent that read eight times
        /// over on pictures nothing ever looked at — measured at 1.2 ms each, on the frame a cell
        /// arrives, which is the frame with the least room for it.
        const osg::Image* getMapImage(int x, int y);

        /// What the widget over a tile darkens it with, or null where the cell has no fog state.
        MyGUI::ITexture* getFogOfWarTexture(int x, int y);

        /**
         * Set the position & direction of the player, and returns the position in map space through the reference
         * parameters.
         * @remarks This is used to draw a "fog of war" effect
         * to hide areas on the map the player has not discovered yet.
         */
        void updatePlayer(const osg::Vec3f& position, const osg::Quat& orientation, float& u, float& v, int& x, int& y,
            osg::Vec3f& direction);

        /**
         * Save the fog of war for this cell to its CellStore.
         * @remarks This should be called when unloading a cell, and for all active cells prior to saving the game.
         */
        void saveFogOfWar(MWWorld::CellStore* cell) const;

        /**
         * Get the interior map texture index and normalized position on this texture, given a world position
         */
        void worldToInteriorMapPosition(osg::Vec2f pos, float& nX, float& nY, int& x, int& y) const;

        osg::Vec2f interiorMapToWorldPosition(float nX, float nY, int x, int y) const;

        /**
         * Check if a given position is explored by the player (i.e. not obscured by fog of war)
         */
        bool isPositionExplored(float nX, float nY, int x, int y);

        MyGUI::IntRect getInteriorGrid() const;

    private:
        Renderer& mRenderer;
        osg::ref_ptr<osg::Node> mSceneRoot;

        enum NeighbourCellFlag : std::uint8_t
        {
            NeighbourCellTopLeft = 1,
            NeighbourCellTopCenter = 1 << 1,
            NeighbourCellTopRight = 1 << 2,
            NeighbourCellMiddleLeft = 1 << 3,
            NeighbourCellMiddleRight = 1 << 4,
            NeighbourCellBottomLeft = 1 << 5,
            NeighbourCellBottomCenter = 1 << 6,
            NeighbourCellBottomRight = 1 << 7,
        };

        struct MapSegment
        {
            void initFogOfWar();
            void loadFogOfWar(const ESM::FogTexture& fog);
            void saveFogOfWar(ESM::FogTexture& fog) const;

            /// The fog image is written a texel at a time as the player walks; this is what puts the
            /// result in front of the GUI, and it is called only on the frames that changed it.
            void showFogOfWar();

            std::uint8_t mLastRenderNeighbourFlags = 0;
            bool mHasFogState = false;

            /// Whether anything has asked this segment for a copy in main memory. Only the world
            /// map does, and only for the cell the player walked into.
            bool mCopyAsked = false;

            /// The picture of this piece of the world, drawn once when the cell is entered and
            /// again when a neighbour arriving makes a better one possible.
            std::unique_ptr<OffscreenView> mView;

            MyGUIPlatform::Picture mFogOfWar{ "fog of war" };
            osg::ref_ptr<osg::Image> mFogOfWarImage;
        };

        typedef std::map<std::pair<int, int>, MapSegment> SegmentMap;
        SegmentMap mExteriorSegments;
        SegmentMap mInteriorSegments;

        int mMapResolution;

        // the dynamic texture is a bottleneck, so don't set this too high
        static const int sFogOfWarResolution = 32;

        // size of a map segment (for exteriors, 1 cell)
        int mMapWorldSize;

        int mCellDistance;

        float mAngle;
        const osg::Vec2f rotatePoint(const osg::Vec2f& point, const osg::Vec2f& center, const float angle) const;

        void requestExteriorMap(const MWWorld::CellStore* cell, MapSegment& segment);
        void requestInteriorMap(const MWWorld::CellStore* cell);

        /// The segment's view, made the first time it is asked for and redrawn every time after.
        void draw(int segmentX, int segmentY, float left, float top, const osg::Vec3d& upVector);

        osg::BoundingBox mBounds;
        osg::Vec2f mCenter;
        bool mInterior;

        std::uint8_t getExteriorNeighbourFlags(int cellX, int cellY) const;
    };

}
#endif

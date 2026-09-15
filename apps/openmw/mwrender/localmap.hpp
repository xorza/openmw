#ifndef GAME_RENDER_LOCALMAP_H
#define GAME_RENDER_LOCALMAP_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <MyGUI_Types.h>
#include <osg/BoundingBox>
#include <osg/BoundingSphere>
#include <osg/Quat>
#include <osg/ref_ptr>

namespace MWWorld
{
    class CellStore;
}

namespace ESM
{
    struct FogTexture;
}

namespace osg
{
    class Texture2D;
    class Image;
    class Node;
}

namespace Terrain
{
    class Storage;
}

namespace MWRender
{
    class OffscreenView;
    class Renderer;

    /// The heights a picture taken straight down has to reach between.
    struct DepthRange
    {
        float mMin = 0.f;
        float mMax = 0.f;

        bool operator==(const DepthRange&) const = default;
    };

    /// What an exterior tile is drawn between: the loaded scene's bound, as upstream read it, and
    /// the cell's own land under it — which is in that bound only where a renderer builds the
    /// ground into the graph, and the ray tracer does not. `land` is nothing for a cell with no
    /// land record.
    DepthRange mapDepthRange(const osg::BoundingSphere& scene, const std::optional<DepthRange>& land);

    ///
    /// \brief Local map rendering
    ///
    class LocalMap
    {
    public:
        /// @param sceneRoot what a tile is a picture of, and what its interior bounds are read off
        /// @param storage where an exterior tile reads its land's heights
        LocalMap(Renderer& renderer, osg::Node& sceneRoot, Terrain::Storage& storage);
        ~LocalMap();

        /**
         * Clear all savegame-specific data (i.e. fog of war textures)
         */
        void clear();

        /**
         * Request a map render for the given cell. Render textures will be immediately created and can be retrieved
         * with the getMapView function.
         */
        void requestMap(const MWWorld::CellStore* cell);

        void addCell(MWWorld::CellStore* cell);
        void removeExteriorCell(int x, int y);

        void removeCell(MWWorld::CellStore* cell);

        /// The picture of this segment, for a widget to show, or null where the cell has not been mapped. Shared,
        /// so that a widget still showing it keeps it alive after the segment has been dropped or redrawn into a
        /// new view.
        std::shared_ptr<const OffscreenView> getMapView(int x, int y);

        osg::ref_ptr<osg::Texture2D> getFogOfWarTexture(int x, int y);

        /// The same picture in main memory, for the global map, or null while it has not come back off the device
        /// yet: ask again next frame. Asking is what starts the copy, so only the tile the world map asks for pays it.
        const osg::Image* getMapImage(int x, int y);

        /// How far from the eye the ground is built, or 0 where it reaches no further than the loaded cells
        float getGroundReach() const;

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
        Terrain::Storage& mStorage;

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
            void createFogOfWarTexture();

            std::uint8_t mLastRenderNeighbourFlags = 0;
            bool mHasFogState = false;

            std::shared_ptr<OffscreenView> mView;

            // What mView was described with; another range rebuilds it
            DepthRange mRange;

            osg::ref_ptr<osg::Texture2D> mFogOfWarTexture;
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

        void draw(
            int segmentX, int segmentY, float left, float top, const osg::Vec3d& upVector, const DepthRange& range);

        osg::BoundingBox mBounds;
        osg::Vec2f mCenter;
        bool mInterior;

        std::uint8_t getExteriorNeighbourFlags(int cellX, int cellY) const;
    };

}
#endif

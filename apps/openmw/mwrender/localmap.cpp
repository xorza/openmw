#include "localmap.hpp"

#include <cstdint>

#include <osg/ComputeBoundsVisitor>
#include <osg/Image>
#include <osg/Texture2D>

#include <osgDB/ReadFile>

#include <components/debug/debuglog.hpp>
#include <components/esm3/fogstate.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/files/memorystream.hpp>
#include <components/misc/constants.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwworld/cellstore.hpp"

#include "offscreenview.hpp"
#include "renderer.hpp"
#include "vismask.hpp"

namespace
{
    float square(float val)
    {
        return val * val;
    }

    /// **The map camera hangs at a fixed height and looks through a fixed slab**, rather than being
    /// fitted to whatever happens to be loaded. A tile should not change because a neighbouring cell
    /// arrived, and a projection that never changes is what lets a segment keep one picture and
    /// redraw it instead of building a new one. Orthographic depth is linear, so even a slab this
    /// deep still resolves to a hundredth of a unit in a 24-bit buffer.
    constexpr float sMapEyeHeight = 50000.f;
    constexpr float sMapNear = 5.f;
    constexpr float sMapFar = 150000.f;

    std::pair<int, int> divideIntoSegments(const osg::BoundingBox& bounds, int mapSize)
    {
        osg::Vec2f min(bounds.xMin(), bounds.yMin());
        osg::Vec2f max(bounds.xMax(), bounds.yMax());
        osg::Vec2f length = max - min;
        const int segsX = static_cast<int>(std::ceil(length.x() / mapSize));
        const int segsY = static_cast<int>(std::ceil(length.y() / mapSize));
        return { segsX, segsY };
    }
}

namespace MWRender
{
    LocalMap::LocalMap(Renderer& renderer, osg::Group& root)
        : mRenderer(renderer)
        , mMapResolution(static_cast<int>(
              Settings::map().mLocalMapResolution * MWBase::Environment::get().getWindowManager()->getScalingFactor()))
        , mMapWorldSize(Constants::CellSizeInUnits)
        , mCellDistance(Constants::CellGridRadius)
        , mAngle(0.f)
        , mInterior(false)
    {
        SceneUtil::FindByNameVisitor find("Scene Root");
        root.accept(find);
        mSceneRoot = find.mFoundNode;
        if (!mSceneRoot)
            throw std::runtime_error("no scene root found");
    }

    LocalMap::~LocalMap() = default;

    const osg::Vec2f LocalMap::rotatePoint(const osg::Vec2f& point, const osg::Vec2f& center, const float angle) const
    {
        return osg::Vec2f(
            std::cos(angle) * (point.x() - center.x()) - std::sin(angle) * (point.y() - center.y()) + center.x(),
            std::sin(angle) * (point.x() - center.x()) + std::cos(angle) * (point.y() - center.y()) + center.y());
    }

    void LocalMap::clear()
    {
        mExteriorSegments.clear();
        mInteriorSegments.clear();
    }

    void LocalMap::saveFogOfWar(MWWorld::CellStore* cell) const
    {
        if (!mInterior)
        {
            const auto it
                = mExteriorSegments.find(std::make_pair(cell->getCell()->getGridX(), cell->getCell()->getGridY()));
            if (it == mExteriorSegments.end())
                return;
            const MapSegment& segment = it->second;

            if (segment.mFogOfWarImage && segment.mHasFogState)
            {
                auto fog = std::make_unique<ESM::FogState>();
                fog->mFogTextures.emplace_back();

                segment.saveFogOfWar(fog->mFogTextures.back());

                cell->setFog(std::move(fog));
            }
        }
        else
        {
            auto segments = divideIntoSegments(mBounds, mMapWorldSize);

            auto fog = std::make_unique<ESM::FogState>();

            fog->mBounds.mMinX = mBounds.xMin();
            fog->mBounds.mMaxX = mBounds.xMax();
            fog->mBounds.mMinY = mBounds.yMin();
            fog->mBounds.mMaxY = mBounds.yMax();
            fog->mNorthMarkerAngle = mAngle;
            fog->mCenterX = mCenter.x();
            fog->mCenterY = mCenter.y();

            fog->mFogTextures.reserve(segments.first * segments.second);

            for (int x = 0; x < segments.first; ++x)
            {
                for (int y = 0; y < segments.second; ++y)
                {
                    const auto it = mInteriorSegments.find(std::make_pair(x, y));
                    if (it == mInteriorSegments.end())
                        continue;
                    const MapSegment& segment = it->second;
                    if (!segment.mHasFogState)
                        continue;
                    ESM::FogTexture& texture = fog->mFogTextures.emplace_back();
                    segment.saveFogOfWar(texture);
                    texture.mX = x;
                    texture.mY = y;
                }
            }

            cell->setFog(std::move(fog));
        }
    }

    void LocalMap::draw(int segmentX, int segmentY, float left, float top, const osg::Vec3d& upVector)
    {
        MapSegment& segment = mInterior ? mInteriorSegments[std::make_pair(segmentX, segmentY)]
                                        : mExteriorSegments[std::make_pair(segmentX, segmentY)];

        if (!segment.mView)
        {
            OffscreenViewSpec spec{ *mSceneRoot };
            spec.mWidth = mMapResolution;
            spec.mHeight = mMapResolution;
            // **The rasterizer's cull mask, and nothing to the ray tracer.** A map tile is a
            // picture of the world, so the trace runs against the scene the frame's own walk built
            // and this selects nothing — see `OffscreenViewSpec::mMask`. It is an inclusion mask
            // either way, which is deliberate here: a chart wants the ground and the buildings and
            // not the smoke over them.
            spec.mMask = Mask_Scene | Mask_SimpleWater | Mask_Terrain | Mask_Object | Mask_Static;
            spec.mProjection = OffscreenViewSpec::Orthographic{ .mWidth = static_cast<float>(mMapWorldSize),
                .mHeight = static_cast<float>(mMapWorldSize) };
            spec.mNear = sMapNear;
            spec.mFar = sMapFar;
            spec.mClearColour = osg::Vec4f(0.f, 0.f, 0.f, 1.f);
            // Flat and from nowhere in particular: a chart is read for what is where, and a sun
            // angle that made shadows would only make it harder to read.
            spec.mSunDirection = osg::Vec3f(-0.3f, -0.3f, 0.7f);
            spec.mSunDiffuse = osg::Vec4f(0.7f, 0.7f, 0.7f, 1.f);
            spec.mSunAmbient = osg::Vec4f(0.3f, 0.3f, 0.3f, 1.f);
            spec.mFromWorld = true;

            segment.mView = mRenderer.createOffscreenView(spec);
        }

        segment.mView->setView(osg::Matrixf::lookAt(
            osg::Vec3f(left, top, sMapEyeHeight), osg::Vec3f(left, top, sMapEyeHeight - 1.f), osg::Vec3f(upVector)));
        segment.mView->redraw();
    }

    void LocalMap::requestMap(const MWWorld::CellStore* cell)
    {
        if (!cell->isExterior())
        {
            requestInteriorMap(cell);
            return;
        }

        int cellX = cell->getCell()->getGridX();
        int cellY = cell->getCell()->getGridY();

        MapSegment& segment = mExteriorSegments[std::make_pair(cellX, cellY)];
        const std::uint8_t neighbourFlags = getExteriorNeighbourFlags(cellX, cellY);
        if (segment.mLastRenderNeighbourFlags != 0
            && (segment.mLastRenderNeighbourFlags & neighbourFlags) == neighbourFlags)
            return;
        requestExteriorMap(cell, segment);
        segment.mLastRenderNeighbourFlags = neighbourFlags;
    }

    void LocalMap::addCell(MWWorld::CellStore* cell)
    {
        if (cell->isExterior())
            mExteriorSegments.emplace(
                std::make_pair(cell->getCell()->getGridX(), cell->getCell()->getGridY()), MapSegment{});
    }

    void LocalMap::removeExteriorCell(int x, int y)
    {
        mExteriorSegments.erase({ x, y });
    }

    void LocalMap::removeCell(MWWorld::CellStore* cell)
    {
        saveFogOfWar(cell);

        if (!cell->isExterior())
            mInteriorSegments.clear();
    }

    MyGUI::ITexture* LocalMap::getMapTexture(int x, int y)
    {
        auto& segments(mInterior ? mInteriorSegments : mExteriorSegments);
        SegmentMap::iterator found = segments.find(std::make_pair(x, y));
        if (found == segments.end() || !found->second.mView)
            return nullptr;

        return &found->second.mView->getTexture();
    }

    const osg::Image* LocalMap::getMapImage(int x, int y)
    {
        SegmentMap::iterator found = mExteriorSegments.find(std::make_pair(x, y));
        if (found == mExteriorSegments.end() || !found->second.mView)
            return nullptr;

        MapSegment& segment = found->second;

        // **Once, and the answer arrives later.** A view keeps no copy until something asks, and
        // filling one takes a draw — so the first ask starts it and comes back with nothing, and
        // the caller asks again. Asking again must not redraw: the rasterizer's copy takes a
        // couple of frames to land and a redraw every frame would keep resetting the wait.
        if (!segment.mCopyAsked)
        {
            segment.mCopyAsked = true;
            segment.mView->keepCopy();
            segment.mView->redraw();
        }

        return segment.mView->getCopy();
    }

    MyGUI::ITexture* LocalMap::getFogOfWarTexture(int x, int y)
    {
        auto& segments(mInterior ? mInteriorSegments : mExteriorSegments);
        SegmentMap::iterator found = segments.find(std::make_pair(x, y));
        if (found == segments.end())
            return nullptr;

        return found->second.mFogOfWar.getTexture();
    }

    void LocalMap::requestExteriorMap(const MWWorld::CellStore* cell, MapSegment& segment)
    {
        mInterior = false;

        const int x = cell->getCell()->getGridX();
        const int y = cell->getCell()->getGridY();

        draw(x, y, x * mMapWorldSize + mMapWorldSize / 2.f, y * mMapWorldSize + mMapWorldSize / 2.f,
            osg::Vec3d(0, 1, 0));

        if (segment.mFogOfWarImage != nullptr)
            return;

        if (cell->getFog() && !cell->getFog()->mFogTextures.empty())
            segment.loadFogOfWar(cell->getFog()->mFogTextures.back());
        else
            segment.initFogOfWar();
    }

    static osg::Vec2f getNorthVector(const MWWorld::CellStore* cell)
    {
        MWWorld::ConstPtr northmarker = cell->searchConst(ESM::RefId::stringRefId("northmarker"));

        if (northmarker.isEmpty())
            return osg::Vec2f(0, 1);

        osg::Quat orient(-northmarker.getRefData().getPosition().rot[2], osg::Vec3f(0, 0, 1));
        osg::Vec3f dir = orient * osg::Vec3f(0, 1, 0);
        osg::Vec2f d(dir.x(), dir.y());
        return d;
    }

    void LocalMap::requestInteriorMap(const MWWorld::CellStore* cell)
    {
        osg::ComputeBoundsVisitor computeBoundsVisitor;
        computeBoundsVisitor.setTraversalMask(Mask_Scene | Mask_Terrain | Mask_Object | Mask_Static);
        mSceneRoot->accept(computeBoundsVisitor);

        osg::BoundingBox bounds = computeBoundsVisitor.getBoundingBox();

        // If we're in an empty cell, bail out
        // The operations in this function are only valid for finite bounds
        if (!bounds.valid() || bounds.radius2() == 0.0)
            return;

        mInterior = true;
        mExteriorSegments.clear();

        mBounds = bounds;

        // Get the cell's NorthMarker rotation. This is used to rotate the entire map.
        osg::Vec2f north = getNorthVector(cell);

        mAngle = std::atan2(north.x(), north.y());

        // Rotate the cell and merge the rotated corners to the bounding box
        osg::Vec2f origCenter(bounds.center().x(), bounds.center().y());
        osg::Vec3f origCorners[8];
        for (int i = 0; i < 8; ++i)
            origCorners[i] = mBounds.corner(i);

        for (int i = 0; i < 8; ++i)
        {
            osg::Vec3f corner = origCorners[i];
            osg::Vec2f corner2d(corner.x(), corner.y());
            corner2d = rotatePoint(corner2d, origCenter, mAngle);
            mBounds.expandBy(osg::Vec3f(corner2d.x(), corner2d.y(), 0));
        }

        // Do NOT change padding! This will break older savegames.
        // If the padding really needs to be changed, then it must be saved in the ESM::FogState and
        // assume the old (500) value as default for older savegames.
        const float padding = 500.0f;

        // Apply a little padding
        mBounds.set(mBounds._min - osg::Vec3f(padding, padding, 0.f), mBounds._max + osg::Vec3f(padding, padding, 0.f));

        mCenter = osg::Vec2f(mBounds.center().x(), mBounds.center().y());

        // If there is fog state in the CellStore (e.g. when it came from a savegame) we need to do some checks
        // to see if this state is still valid.
        // Both the cell bounds and the NorthMarker rotation could be changed by the content files or exchanged models.
        // If they changed by too much then parts of the interior might not be covered by the map anymore.
        // The following code detects this, and discards the CellStore's fog state if it needs to.
        int xOffset = 0;
        int yOffset = 0;
        if (const ESM::FogState* fog = cell->getFog())
        {
            if (std::abs(mAngle - fog->mNorthMarkerAngle) < osg::DegreesToRadians(5.f))
            {
                // Expand mBounds so the saved textures fit the same grid
                if (fog->mBounds.mMinX < mBounds.xMin())
                {
                    mBounds.xMin() = fog->mBounds.mMinX;
                }
                else if (fog->mBounds.mMinX > mBounds.xMin())
                {
                    float diff = fog->mBounds.mMinX - mBounds.xMin();
                    xOffset = static_cast<int>(std::ceil(diff / mMapWorldSize));
                    mBounds.xMin() = fog->mBounds.mMinX - xOffset * mMapWorldSize;
                }
                if (fog->mBounds.mMinY < mBounds.yMin())
                {
                    mBounds.yMin() = fog->mBounds.mMinY;
                }
                else if (fog->mBounds.mMinY > mBounds.yMin())
                {
                    float diff = fog->mBounds.mMinY - mBounds.yMin();
                    yOffset = static_cast<int>(std::ceil(diff / mMapWorldSize));
                    mBounds.yMin() = fog->mBounds.mMinY - yOffset * mMapWorldSize;
                }
                if (fog->mBounds.mMaxX > mBounds.xMax())
                    mBounds.xMax() = fog->mBounds.mMaxX;
                if (fog->mBounds.mMaxY > mBounds.yMax())
                    mBounds.yMax() = fog->mBounds.mMaxY;

                if (xOffset != 0 || yOffset != 0)
                    Log(Debug::Warning) << "Warning: expanding fog by " << xOffset << ", " << yOffset;

                mAngle = fog->mNorthMarkerAngle;
                mCenter.x() = fog->mCenterX;
                mCenter.y() = fog->mCenterY;
            }
        }

        osg::Vec2f min(mBounds.xMin(), mBounds.yMin());

        osg::Quat cameraOrient(mAngle, osg::Vec3d(0, 0, -1));

        auto segments = divideIntoSegments(mBounds, mMapWorldSize);
        for (int x = 0; x < segments.first; ++x)
        {
            for (int y = 0; y < segments.second; ++y)
            {
                osg::Vec2f start
                    = min + osg::Vec2f(static_cast<float>(mMapWorldSize * x), static_cast<float>(mMapWorldSize * y));
                osg::Vec2f newcenter = start + osg::Vec2f(mMapWorldSize / 2.f, mMapWorldSize / 2.f);

                osg::Vec2f a = newcenter - mCenter;
                osg::Vec3f rotatedCenter = cameraOrient * (osg::Vec3f(a.x(), a.y(), 0));

                osg::Vec2f pos = osg::Vec2f(rotatedCenter.x(), rotatedCenter.y()) + mCenter;

                draw(x, y, pos.x(), pos.y(), osg::Vec3f(north.x(), north.y(), 0.f));

                auto coords = std::make_pair(x, y);
                MapSegment& segment = mInteriorSegments[coords];
                if (!segment.mFogOfWarImage)
                {
                    bool loaded = false;
                    if (const ESM::FogState* fog = cell->getFog())
                    {
                        auto match = std::find_if(
                            fog->mFogTextures.begin(), fog->mFogTextures.end(), [&](const ESM::FogTexture& texture) {
                                return texture.mX == x - xOffset && texture.mY == y - yOffset;
                            });
                        if (match != fog->mFogTextures.end())
                        {
                            segment.loadFogOfWar(*match);
                            loaded = true;
                        }
                    }
                    if (!loaded)
                        segment.initFogOfWar();
                }
            }
        }
    }

    void LocalMap::worldToInteriorMapPosition(osg::Vec2f pos, float& nX, float& nY, int& x, int& y) const
    {
        pos = rotatePoint(pos, mCenter, mAngle);

        osg::Vec2f min(mBounds.xMin(), mBounds.yMin());

        x = static_cast<int>(std::ceil((pos.x() - min.x()) / mMapWorldSize) - 1);
        y = static_cast<int>(std::ceil((pos.y() - min.y()) / mMapWorldSize) - 1);

        nX = (pos.x() - min.x() - mMapWorldSize * x) / mMapWorldSize;
        nY = 1.0f - (pos.y() - min.y() - mMapWorldSize * y) / mMapWorldSize;
    }

    osg::Vec2f LocalMap::interiorMapToWorldPosition(float nX, float nY, int x, int y) const
    {
        osg::Vec2f min(mBounds.xMin(), mBounds.yMin());
        osg::Vec2f pos(mMapWorldSize * (nX + x) + min.x(), mMapWorldSize * (1.0f - nY + y) + min.y());

        pos = rotatePoint(pos, mCenter, -mAngle);
        return pos;
    }

    bool LocalMap::isPositionExplored(float nX, float nY, int x, int y)
    {
        auto& segments(mInterior ? mInteriorSegments : mExteriorSegments);
        const MapSegment& segment = segments[std::make_pair(x, y)];
        if (!segment.mFogOfWarImage)
            return false;

        nX = std::clamp(nX, 0.f, 1.f);
        nY = std::clamp(nY, 0.f, 1.f);

        int texU = static_cast<int>((sFogOfWarResolution - 1) * nX);
        int texV = static_cast<int>((sFogOfWarResolution - 1) * nY);

        const std::uint32_t clr
            = reinterpret_cast<const uint32_t*>(segment.mFogOfWarImage->data())[texV * sFogOfWarResolution + texU];
        uint8_t alpha = (clr >> 24);
        return alpha < 200;
    }

    void LocalMap::updatePlayer(const osg::Vec3f& position, const osg::Quat& orientation, float& u, float& v, int& x,
        int& y, osg::Vec3f& direction)
    {
        // retrieve the x,y grid coordinates the player is in
        osg::Vec2f pos(position.x(), position.y());

        if (mInterior)
        {
            worldToInteriorMapPosition(pos, u, v, x, y);

            osg::Quat cameraOrient(mAngle, osg::Vec3(0, 0, -1));
            direction = orientation * cameraOrient.inverse() * osg::Vec3f(0, 1, 0);
        }
        else
        {
            direction = orientation * osg::Vec3f(0, 1, 0);

            x = static_cast<int>(std::ceil(pos.x() / mMapWorldSize) - 1);
            y = static_cast<int>(std::ceil(pos.y() / mMapWorldSize) - 1);

            // convert from world coordinates to texture UV coordinates
            u = std::abs((pos.x() - (mMapWorldSize * x)) / mMapWorldSize);
            v = 1.0f - std::abs((pos.y() - (mMapWorldSize * y)) / mMapWorldSize);
        }

        // explore radius (squared)
        const float exploreRadius = 0.17f * (sFogOfWarResolution - 1); // explore radius from 0 to sFogOfWarResolution-1
        const float sqrExploreRadius = square(exploreRadius);
        const float exploreRadiusUV = exploreRadius / sFogOfWarResolution; // explore radius from 0 to 1 (UV space)

        // change the affected fog of war textures (in a 3x3 grid around the player)
        for (int mx = -mCellDistance; mx <= mCellDistance; ++mx)
        {
            for (int my = -mCellDistance; my <= mCellDistance; ++my)
            {
                // is this texture affected at all?
                bool affected = false;
                if (mx == 0 && my == 0) // the player is always in the center of the 3x3 grid
                    affected = true;
                else
                {
                    bool affectsX = (mx > 0) ? (u + exploreRadiusUV > 1) : (u - exploreRadiusUV < 0);
                    bool affectsY = (my > 0) ? (v + exploreRadiusUV > 1) : (v - exploreRadiusUV < 0);
                    affected = (affectsX && (my == 0)) || (affectsY && mx == 0) || (affectsX && affectsY);
                }

                if (!affected)
                    continue;

                int texX = x + mx;
                int texY = y + my * -1;

                auto& segments(mInterior ? mInteriorSegments : mExteriorSegments);
                MapSegment& segment = segments[std::make_pair(texX, texY)];

                if (!segment.mFogOfWarImage || !segment.mView)
                    continue;

                std::uint32_t* data = reinterpret_cast<std::uint32_t*>(segment.mFogOfWarImage->data());
                bool changed = false;
                for (int texV = 0; texV < sFogOfWarResolution; ++texV)
                {
                    for (int texU = 0; texU < sFogOfWarResolution; ++texU)
                    {
                        float sqrDist = square((texU + mx * (sFogOfWarResolution - 1)) - u * (sFogOfWarResolution - 1))
                            + square((texV + my * (sFogOfWarResolution - 1)) - v * (sFogOfWarResolution - 1));

                        const std::uint8_t alpha = std::min<std::uint8_t>(*data >> 24,
                            static_cast<std::uint8_t>(std::clamp(sqrDist / sqrExploreRadius, 0.f, 1.f) * 255));
                        std::uint32_t val = static_cast<std::uint32_t>(alpha << 24);
                        if (*data != val)
                        {
                            *data = val;
                            changed = true;
                        }

                        ++data;
                    }
                }

                if (changed)
                {
                    segment.mHasFogState = true;
                    segment.showFogOfWar();
                }
            }
        }
    }

    std::uint8_t LocalMap::getExteriorNeighbourFlags(int cellX, int cellY) const
    {
        constexpr std::tuple<NeighbourCellFlag, int, int> flags[] = {
            { NeighbourCellTopLeft, -1, -1 },
            { NeighbourCellTopCenter, 0, -1 },
            { NeighbourCellTopRight, 1, -1 },
            { NeighbourCellMiddleLeft, -1, 0 },
            { NeighbourCellMiddleRight, 1, 0 },
            { NeighbourCellBottomLeft, -1, 1 },
            { NeighbourCellBottomCenter, 0, 1 },
            { NeighbourCellBottomRight, 1, 1 },
        };
        std::uint8_t result = 0;
        for (const auto& [flag, dx, dy] : flags)
        {
            auto it = mExteriorSegments.find(std::pair(cellX + dx, cellY + dy));
            if (it != mExteriorSegments.end() && it->second.mView)
                result |= flag;
        }
        return result;
    }

    MyGUI::IntRect LocalMap::getInteriorGrid() const
    {
        auto segments = divideIntoSegments(mBounds, mMapWorldSize);
        return { -1, -1, segments.first, segments.second };
    }

    void LocalMap::MapSegment::showFogOfWar()
    {
        if (mFogOfWarImage)
            mFogOfWar.set(*mFogOfWarImage);
    }

    void LocalMap::MapSegment::initFogOfWar()
    {
        mFogOfWarImage = new osg::Image;
        mFogOfWarImage->allocateImage(sFogOfWarResolution, sFogOfWarResolution, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        assert(mFogOfWarImage->isDataContiguous());
        std::vector<uint32_t> data;
        data.resize(sFogOfWarResolution * sFogOfWarResolution, 0xff000000);

        memcpy(mFogOfWarImage->data(), data.data(), data.size() * 4);

        showFogOfWar();
    }

    void LocalMap::MapSegment::loadFogOfWar(const ESM::FogTexture& esm)
    {
        const std::vector<char>& data = esm.mImageData;
        if (data.empty())
        {
            initFogOfWar();
            return;
        }

        osgDB::ReaderWriter* readerwriter = osgDB::Registry::instance()->getReaderWriterForExtension("png");
        if (!readerwriter)
        {
            Log(Debug::Error) << "Error: Unable to load fog, can't find a png ReaderWriter";
            return;
        }

        Files::IMemStream in(data.data(), data.size());

        osgDB::ReaderWriter::ReadResult result = readerwriter->readImage(in);
        if (!result.success())
        {
            Log(Debug::Error) << "Error: Failed to read fog: " << result.message() << " code " << result.status();
            return;
        }

        mFogOfWarImage = result.getImage();
        mFogOfWarImage->flipVertical();
        mFogOfWarImage->dirty();

        showFogOfWar();
        mHasFogState = true;
    }

    void LocalMap::MapSegment::saveFogOfWar(ESM::FogTexture& fog) const
    {
        if (!mFogOfWarImage)
            return;

        std::ostringstream ostream;

        osgDB::ReaderWriter* readerwriter = osgDB::Registry::instance()->getReaderWriterForExtension("png");
        if (!readerwriter)
        {
            Log(Debug::Error) << "Error: Unable to write fog, can't find a png ReaderWriter";
            return;
        }

        // extra flips are unfortunate, but required for compatibility with older versions
        mFogOfWarImage->flipVertical();
        osgDB::ReaderWriter::WriteResult result = readerwriter->writeImage(*mFogOfWarImage, ostream);
        if (!result.success())
        {
            Log(Debug::Error) << "Error: Unable to write fog: " << result.message() << " code " << result.status();
            return;
        }
        mFogOfWarImage->flipVertical();

        std::string data = ostream.str();
        fog.mImageData = std::vector<char>(data.begin(), data.end());
    }

}

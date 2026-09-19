#include "globalmap.hpp"

#include <osg/Image>
#include <osg/Texture2D>

#include <osgDB/WriteFile>

#include <components/files/memorystream.hpp>
#include <components/settings/values.hpp>

#include <components/debug/debuglog.hpp>

#include <components/resource/imagemanager.hpp>
#include <components/resource/resourcesystem.hpp>

#include <components/sceneutil/imageregion.hpp>
#include <components/sceneutil/workqueue.hpp>

#include <components/vfs/pathutil.hpp>

#include <components/esm3/globalmap.hpp>
#include <components/esm3/loadland.hpp>

#include "../mwbase/environment.hpp"

#include "../mwworld/esmstore.hpp"

#include "mapoverlay.hpp"
#include "renderer.hpp"

namespace
{

    std::vector<char> writePng(const osg::Image& overlayImage)
    {
        std::ostringstream ostream;
        osgDB::ReaderWriter* readerwriter = osgDB::Registry::instance()->getReaderWriterForExtension("png");
        if (!readerwriter)
        {
            Log(Debug::Error) << "Error: Can't write map overlay: no png readerwriter found";
            return std::vector<char>();
        }

        osgDB::ReaderWriter::WriteResult result = readerwriter->writeImage(overlayImage, ostream);
        if (!result.success())
        {
            Log(Debug::Warning) << "Error: Can't write map overlay: " << result.message() << " code "
                                << result.status();
            return std::vector<char>();
        }

        std::string data = ostream.str();
        return std::vector<char>(data.begin(), data.end());
    }
}

namespace MWRender
{

    class CreateMapWorkItem : public SceneUtil::WorkItem
    {
    public:
        CreateMapWorkItem(int width, int height, int minX, int minY, int maxX, int maxY, int cellSize,
            const MWWorld::Store<ESM::Land>& landStore, osg::ref_ptr<osg::Image> colorLut)
            : mWidth(width)
            , mHeight(height)
            , mMinX(minX)
            , mMinY(minY)
            , mMaxX(maxX)
            , mMaxY(maxY)
            , mCellSize(cellSize)
            , mLandStore(landStore)
            , mColorLut(colorLut)
        {
        }

        void doWork() override
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(mWidth, mHeight, 1, GL_RGB, GL_UNSIGNED_BYTE);

            osg::ref_ptr<osg::Image> alphaImage = new osg::Image;
            alphaImage->allocateImage(mWidth, mHeight, 1, GL_ALPHA, GL_UNSIGNED_BYTE);

            for (int x = mMinX; x <= mMaxX; ++x)
            {
                for (int y = mMinY; y <= mMaxY; ++y)
                {
                    const ESM::Land* land = mLandStore.search(x, y);

                    for (int cellY = 0; cellY < mCellSize; ++cellY)
                    {
                        for (int cellX = 0; cellX < mCellSize; ++cellX)
                        {
                            int vertexX = (cellX * 9) / mCellSize; // 0..8
                            int vertexY = (cellY * 9) / mCellSize; // 0..8

                            int texelX = (x - mMinX) * mCellSize + cellX;
                            int texelY = (y - mMinY) * mCellSize + cellY;

                            int lutIndex = 0;
                            // Converting [-128; 127] WNAM range to [0; 255] index
                            if (land != nullptr && (land->mDataTypes & ESM::Land::DATA_WNAM))
                                lutIndex = static_cast<int>(land->mWnam[vertexY * 9 + vertexX]) + 128;

                            // Use getColor to handle all pixel format conversions automatically
                            osg::Vec4 color = mColorLut->getColor(lutIndex, 0);

                            // Use setColor to write to output images
                            image->setColor(color, texelX, texelY);

                            // Set alpha based on lutIndex threshold
                            osg::Vec4 alpha(0.0f, 0.0f, 0.0f, lutIndex < 128 ? 0.0f : 1.0f);
                            alphaImage->setColor(alpha, texelX, texelY);
                        }
                    }
                }
            }

            mBaseTexture = new osg::Texture2D;
            mBaseTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            mBaseTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            mBaseTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            mBaseTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            mBaseTexture->setImage(image);
            mBaseTexture->setResizeNonPowerOfTwoHint(false);

            mAlphaImage = alphaImage;
        }

        int mWidth, mHeight;
        int mMinX, mMinY, mMaxX, mMaxY;
        int mCellSize;
        const MWWorld::Store<ESM::Land>& mLandStore;
        osg::ref_ptr<osg::Image> mColorLut;

        osg::ref_ptr<osg::Texture2D> mBaseTexture;
        osg::ref_ptr<osg::Image> mAlphaImage;
    };

    struct GlobalMap::WritePng final : public SceneUtil::WorkItem
    {
        osg::ref_ptr<const osg::Image> mOverlayImage;
        std::vector<char> mImageData;

        explicit WritePng(osg::ref_ptr<const osg::Image> overlayImage)
            : mOverlayImage(std::move(overlayImage))
        {
        }

        void doWork() override { mImageData = writePng(*mOverlayImage); }
    };

    GlobalMap::GlobalMap(Renderer& renderer, SceneUtil::WorkQueue* workQueue)
        : mRenderer(renderer)
        , mWorkQueue(workQueue)
        , mWidth(0)
        , mHeight(0)
        , mMinX(0)
        , mMaxX(0)
        , mMinY(0)
        , mMaxY(0)
    {
    }

    GlobalMap::~GlobalMap()
    {
        if (mWorkItem)
            mWorkItem->waitTillDone();
    }

    void GlobalMap::render()
    {
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();

        // get the size of the world
        MWWorld::Store<ESM::Cell>::iterator it = esmStore.get<ESM::Cell>().extBegin();
        for (; it != esmStore.get<ESM::Cell>().extEnd(); ++it)
        {
            if (it->getGridX() < mMinX)
                mMinX = it->getGridX();
            if (it->getGridX() > mMaxX)
                mMaxX = it->getGridX();
            if (it->getGridY() < mMinY)
                mMinY = it->getGridY();
            if (it->getGridY() > mMaxY)
                mMaxY = it->getGridY();
        }

        const int cellSize = Settings::map().mGlobalMapCellSize;

        mWidth = cellSize * (mMaxX - mMinX + 1);
        mHeight = cellSize * (mMaxY - mMinY + 1);

        // Load color LUT texture
        constexpr VFS::Path::NormalizedView colorLutPath("textures/omw_map_color_palette.dds");
        auto resourceSystem = MWBase::Environment::get().getResourceSystem();
        osg::ref_ptr<osg::Image> colorLut = resourceSystem->getImageManager()->getImage(colorLutPath);

        // Validate LUT dimensions
        if (!colorLut || colorLut->s() != 256 || colorLut->t() != 1)
        {
            throw std::runtime_error("Global map color LUT must be 256x1 pixels, got "
                + std::to_string(colorLut ? colorLut->s() : 0) + "x" + std::to_string(colorLut ? colorLut->t() : 0));
        }

        mWorkItem = new CreateMapWorkItem(
            mWidth, mHeight, mMinX, mMinY, mMaxX, mMaxY, cellSize, esmStore.get<ESM::Land>(), colorLut);
        mWorkQueue->addWorkItem(mWorkItem);
    }

    void GlobalMap::worldPosToImageSpace(float x, float z, float& imageX, float& imageY)
    {
        imageX = (float(x / float(Constants::CellSizeInUnits) - mMinX) / (mMaxX - mMinX + 1)) * getWidth();

        imageY = (1.f - float(z / float(Constants::CellSizeInUnits) - mMinY) / (mMaxY - mMinY + 1)) * getHeight();
    }

    void GlobalMap::exploreCell(int cellX, int cellY, std::shared_ptr<OffscreenView> tile)
    {
        ensureLoaded();

        if (!tile)
            return;

        const int cellSize = Settings::map().mGlobalMapCellSize;
        const int originX = (cellX - mMinX) * cellSize;
        const int originY = (cellY - mMinY) * cellSize;

        if (cellX > mMaxX || cellX < mMinX || cellY > mMaxY || cellY < mMinY)
            return;

        mOverlay->paintTile(SceneUtil::ImageRegion{ originX, originY, cellSize, cellSize }, std::move(tile));
    }

    void GlobalMap::clear()
    {
        ensureLoaded();

        mOverlay->clear();
    }

    void GlobalMap::write(ESM::GlobalMap& map)
    {
        ensureLoaded();

        map.mBounds.mMinX = mMinX;
        map.mBounds.mMaxX = mMaxX;
        map.mBounds.mMinY = mMinY;
        map.mBounds.mMaxY = mMaxY;

        if (mWritePng != nullptr)
        {
            mWritePng->waitTillDone();
            map.mImageData = std::move(mWritePng->mImageData);
            mWritePng = nullptr;
            return;
        }

        map.mImageData = writePng(mOverlay->getImage());
    }

    struct Box
    {
        int mLeft, mTop, mRight, mBottom;

        Box(int left, int top, int right, int bottom)
            : mLeft(left)
            , mTop(top)
            , mRight(right)
            , mBottom(bottom)
        {
        }
        bool operator==(const Box& other) const
        {
            return mLeft == other.mLeft && mTop == other.mTop && mRight == other.mRight && mBottom == other.mBottom;
        }
    };

    void GlobalMap::read(ESM::GlobalMap& map)
    {
        ensureLoaded();

        const ESM::GlobalMap::Bounds& bounds = map.mBounds;

        if (bounds.mMaxX - bounds.mMinX < 0)
            return;
        if (bounds.mMaxY - bounds.mMinY < 0)
            return;

        if (bounds.mMinX > bounds.mMaxX || bounds.mMinY > bounds.mMaxY)
            throw std::runtime_error("invalid map bounds");

        if (map.mImageData.empty())
            return;

        Files::IMemStream istream(map.mImageData.data(), map.mImageData.size());

        osgDB::ReaderWriter* readerwriter = osgDB::Registry::instance()->getReaderWriterForExtension("png");
        if (!readerwriter)
        {
            Log(Debug::Error) << "Error: Can't read map overlay: no png readerwriter found";
            return;
        }

        osgDB::ReaderWriter::ReadResult result = readerwriter->readImage(istream);
        if (!result.success())
        {
            Log(Debug::Error) << "Error: Can't read map overlay: " << result.message() << " code " << result.status();
            return;
        }

        osg::ref_ptr<osg::Image> image = result.getImage();
        int imageWidth = image->s();
        int imageHeight = image->t();

        int xLength = (bounds.mMaxX - bounds.mMinX + 1);
        int yLength = (bounds.mMaxY - bounds.mMinY + 1);

        // Size of one cell in image space
        int cellImageSizeSrc = imageWidth / xLength;
        if (int(imageHeight / yLength) != cellImageSizeSrc)
            throw std::runtime_error("cell size must be quadratic");

        // If cell bounds of the currently loaded content and the loaded savegame do not match,
        // we need to resize source/dest boxes to accommodate
        // This means nonexisting cells will be dropped silently
        const int cellImageSizeDst = Settings::map().mGlobalMapCellSize;

        // Completely off-screen? -> no need to blit anything
        if (bounds.mMaxX < mMinX || bounds.mMaxY < mMinY || bounds.mMinX > mMaxX || bounds.mMinY > mMaxY)
            return;

        int leftDiff = (mMinX - bounds.mMinX);
        int topDiff = (bounds.mMaxY - mMaxY);
        int rightDiff = (bounds.mMaxX - mMaxX);
        int bottomDiff = (mMinY - bounds.mMinY);

        Box srcBox(std::max(0, leftDiff * cellImageSizeSrc), std::max(0, topDiff * cellImageSizeSrc),
            std::min(imageWidth, imageWidth - rightDiff * cellImageSizeSrc),
            std::min(imageHeight, imageHeight - bottomDiff * cellImageSizeSrc));

        Box destBox(std::max(0, -leftDiff * cellImageSizeDst), std::max(0, -topDiff * cellImageSizeDst),
            std::min(mWidth, mWidth + rightDiff * cellImageSizeDst),
            std::min(mHeight, mHeight + bottomDiff * cellImageSizeDst));

        if (srcBox == destBox && imageWidth == mWidth && imageHeight == mHeight)
        {
            mOverlay->replace(std::move(image));
        }
        else
        {
            // Dimensions don't match. This could mean a changed map region, or a changed map resolution.
            // In the latter case, we'll want filtering.
            // The boxes above count rows from the top; the overlay's rectangles count them from the bottom.
            const int srcHeight = srcBox.mBottom - srcBox.mTop;
            const int destHeight = destBox.mBottom - destBox.mTop;
            mOverlay->paintImage(SceneUtil::ImageRegion{ destBox.mLeft, mHeight - destBox.mBottom,
                                     destBox.mRight - destBox.mLeft, destHeight },
                std::move(image),
                SceneUtil::ImageRegion{
                    srcBox.mLeft, imageHeight - srcBox.mBottom, srcBox.mRight - srcBox.mLeft, srcHeight });
        }
    }

    osg::ref_ptr<osg::Texture2D> GlobalMap::getBaseTexture()
    {
        ensureLoaded();
        return mBaseTexture;
    }

    MyGUI::ITexture& GlobalMap::getOverlayTexture()
    {
        ensureLoaded();
        return mOverlay->getTexture();
    }

    void GlobalMap::ensureLoaded()
    {
        if (mWorkItem)
        {
            mWorkItem->waitTillDone();

            mBaseTexture = mWorkItem->mBaseTexture;
            mOverlay = mRenderer.createMapOverlay(
                MapOverlaySpec{ .mWidth = mWidth, .mHeight = mHeight, .mLandAlpha = mWorkItem->mAlphaImage });

            mWorkItem = nullptr;
        }
    }

    void GlobalMap::asyncWritePng()
    {
        if (mOverlay == nullptr)
            return;
        // Use deep copy to avoid any sychronization
        mWritePng = new WritePng(new osg::Image(mOverlay->getImage(), osg::CopyOp::DEEP_COPY_ALL));
        mWorkQueue->addWorkItem(mWritePng, /*front=*/true);
    }
}

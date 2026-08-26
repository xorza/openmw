#include "globalmap.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <osg/Image>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>

#include <MyGUI_ITexture.h>
#include <MyGUI_RenderManager.h>

#include <components/debug/debuglog.hpp>
#include <components/files/memorystream.hpp>
#include <components/misc/constants.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/pathutil.hpp>

#include <components/esm3/globalmap.hpp>
#include <components/esm3/loadland.hpp>

#include "../mwbase/environment.hpp"

#include "../mwworld/esmstore.hpp"

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

    /// Whatever came out of the png reader, as the four tightly packed bytes a pixel that everything
    /// below this expects. Returns the image itself where it is already that.
    osg::ref_ptr<osg::Image> asRgba(osg::ref_ptr<osg::Image> image)
    {
        if (image->getPixelFormat() == GL_RGBA && image->getDataType() == GL_UNSIGNED_BYTE && image->isDataContiguous())
            return image;

        osg::ref_ptr<osg::Image> converted = new osg::Image;
        converted->allocateImage(image->s(), image->t(), 1, GL_RGBA, GL_UNSIGNED_BYTE);

        for (int y = 0; y < image->t(); ++y)
            for (int x = 0; x < image->s(); ++x)
                converted->setColor(image->getColor(x, y), x, y);

        return converted;
    }

    /// Bilinear, because this is only reached when a savegame was written at a different map
    /// resolution and the two grids do not line up at all.
    void resample(const osg::Image& from, int fromX, int fromY, int fromWidth, int fromHeight, osg::Image& into,
        int intoX, int intoY, int intoWidth, int intoHeight)
    {
        for (int y = 0; y < intoHeight; ++y)
        {
            const float v = (y + 0.5f) * fromHeight / intoHeight - 0.5f;
            const int v0 = std::clamp(static_cast<int>(std::floor(v)), 0, fromHeight - 1);
            const int v1 = std::clamp(v0 + 1, 0, fromHeight - 1);
            const float vf = std::clamp(v - v0, 0.f, 1.f);

            for (int x = 0; x < intoWidth; ++x)
            {
                const float u = (x + 0.5f) * fromWidth / intoWidth - 0.5f;
                const int u0 = std::clamp(static_cast<int>(std::floor(u)), 0, fromWidth - 1);
                const int u1 = std::clamp(u0 + 1, 0, fromWidth - 1);
                const float uf = std::clamp(u - u0, 0.f, 1.f);

                const std::uint8_t* a = from.data(fromX + u0, fromY + v0);
                const std::uint8_t* b = from.data(fromX + u1, fromY + v0);
                const std::uint8_t* c = from.data(fromX + u0, fromY + v1);
                const std::uint8_t* d = from.data(fromX + u1, fromY + v1);

                std::uint8_t* out = into.data(intoX + x, intoY + y);
                for (int channel = 0; channel < 4; ++channel)
                {
                    const float top = a[channel] + (b[channel] - a[channel]) * uf;
                    const float bottom = c[channel] + (d[channel] - c[channel]) * uf;
                    out[channel] = static_cast<std::uint8_t>(top + (bottom - top) * vf + 0.5f);
                }
            }
        }
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
            mBaseImage = new osg::Image;
            mBaseImage->allocateImage(mWidth, mHeight, 1, GL_RGB, GL_UNSIGNED_BYTE);

            mAlphaImage = new osg::Image;
            mAlphaImage->allocateImage(mWidth, mHeight, 1, GL_ALPHA, GL_UNSIGNED_BYTE);

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

                            mBaseImage->setColor(color, texelX, texelY);

                            // Below the water line there is nothing an explored tile should be
                            // allowed to paint over.
                            *mAlphaImage->data(texelX, texelY) = lutIndex < 128 ? 0 : 0xFF;
                        }
                    }
                }
            }

            mOverlayImage = new osg::Image;
            mOverlayImage->allocateImage(mWidth, mHeight, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            assert(mOverlayImage->isDataContiguous());

            memset(mOverlayImage->data(), 0, mOverlayImage->getTotalSizeInBytes());
        }

        int mWidth, mHeight;
        int mMinX, mMinY, mMaxX, mMaxY;
        int mCellSize;
        const MWWorld::Store<ESM::Land>& mLandStore;
        osg::ref_ptr<osg::Image> mColorLut;

        osg::ref_ptr<osg::Image> mBaseImage;
        osg::ref_ptr<osg::Image> mAlphaImage;
        osg::ref_ptr<osg::Image> mOverlayImage;
    };

    struct GlobalMap::WritePng : public SceneUtil::WorkItem
    {
        explicit WritePng(osg::ref_ptr<const osg::Image> overlayImage)
            : mOverlayImage(std::move(overlayImage))
        {
        }

        void doWork() override { mImageData = writePng(*mOverlayImage); }

        osg::ref_ptr<const osg::Image> mOverlayImage;
        std::vector<char> mImageData;
    };

    GlobalMap::GlobalMap(SceneUtil::WorkQueue* workQueue)
        : mWorkQueue(workQueue)
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

    bool GlobalMap::exploreCell(int cellX, int cellY, const osg::Image* tile)
    {
        ensureLoaded();

        if (cellX > mMaxX || cellX < mMinX || cellY > mMaxY || cellY < mMinY)
            return true;

        if (tile == nullptr)
            return false;

        assert(tile->getPixelFormat() == GL_RGBA && tile->getDataType() == GL_UNSIGNED_BYTE);

        const int cellSize = Settings::map().mGlobalMapCellSize;
        const int originX = (cellX - mMinX) * cellSize;
        const int originY = (cellY - mMinY) * cellSize;

        const int tileWidth = tile->s();
        const int tileHeight = tile->t();

        mCellScratch.resize(static_cast<std::size_t>(cellSize) * cellSize * 4);

        for (int y = 0; y < cellSize; ++y)
        {
            for (int x = 0; x < cellSize; ++x)
            {
                const int left = x * tileWidth / cellSize;
                const int right = std::max(left + 1, (x + 1) * tileWidth / cellSize);
                const int bottom = y * tileHeight / cellSize;
                const int top = std::max(bottom + 1, (y + 1) * tileHeight / cellSize);

                unsigned int sum[4] = { 0, 0, 0, 0 };
                for (int sampleY = bottom; sampleY < top; ++sampleY)
                {
                    const std::uint8_t* row = tile->data(left, sampleY);
                    for (int sampleX = left; sampleX < right; ++sampleX, row += 4)
                        for (int channel = 0; channel < 4; ++channel)
                            sum[channel] += row[channel];
                }

                const unsigned int taken = (right - left) * (top - bottom);
                const unsigned int mask = *mAlphaImage->data(originX + x, originY + y);

                std::uint8_t* out = mCellScratch.data() + (static_cast<std::size_t>(y) * cellSize + x) * 4;
                out[0] = static_cast<std::uint8_t>(sum[0] / taken);
                out[1] = static_cast<std::uint8_t>(sum[1] / taken);
                out[2] = static_cast<std::uint8_t>(sum[2] / taken);
                out[3] = static_cast<std::uint8_t>(sum[3] / taken * mask / 255);
            }
        }

        // Crossing back into a cell asks for it to be explored again, and almost always paints
        // exactly what is already there. Finding that out costs a kilobyte of comparison; acting on
        // it would cost the whole overlay going back up to the device.
        bool changed = false;
        for (int y = 0; y < cellSize && !changed; ++y)
            changed = std::memcmp(mOverlayImage->data(originX, originY + y),
                          mCellScratch.data() + static_cast<std::size_t>(y) * cellSize * 4, cellSize * 4)
                != 0;

        if (!changed)
            return true;

        for (int y = 0; y < cellSize; ++y)
            std::memcpy(mOverlayImage->data(originX, originY + y),
                mCellScratch.data() + static_cast<std::size_t>(y) * cellSize * 4, cellSize * 4);

        // **The cell and not the overlay.** Eighteen pixels square against two megabytes, on the
        // frame a cell arrives. A backend that cannot take a rectangle still gets the whole image,
        // which is what this did unconditionally.
        mOverlay.setRegion(*mOverlayImage, originX, originY, cellSize, cellSize);
        return true;
    }

    void GlobalMap::clear()
    {
        ensureLoaded();

        std::memset(mOverlayImage->data(), 0, mOverlayImage->getTotalSizeInBytes());

        mOverlay.set(*mOverlayImage);
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

        map.mImageData = writePng(*mOverlayImage);
    }

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

        osg::ref_ptr<osg::Image> image = asRgba(result.getImage());
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
            mOverlayImage = image;
        }
        else
        {
            // The boxes above count rows from the top; the images count them from the bottom.
            const int srcHeight = srcBox.mBottom - srcBox.mTop;
            const int destHeight = destBox.mBottom - destBox.mTop;

            std::memset(mOverlayImage->data(), 0, mOverlayImage->getTotalSizeInBytes());

            resample(*image, srcBox.mLeft, imageHeight - srcBox.mBottom, srcBox.mRight - srcBox.mLeft, srcHeight,
                *mOverlayImage, destBox.mLeft, mHeight - destBox.mBottom, destBox.mRight - destBox.mLeft, destHeight);
        }

        mOverlay.set(*mOverlayImage);
    }

    MyGUI::ITexture& GlobalMap::getBaseTexture()
    {
        ensureLoaded();
        return *mBase.getTexture();
    }

    MyGUI::ITexture& GlobalMap::getOverlayTexture()
    {
        ensureLoaded();
        return *mOverlay.getTexture();
    }

    void GlobalMap::ensureLoaded()
    {
        if (!mWorkItem)
            return;

        mWorkItem->waitTillDone();

        const osg::ref_ptr<osg::Image> base = mWorkItem->mBaseImage;
        mAlphaImage = mWorkItem->mAlphaImage;
        mOverlayImage = mWorkItem->mOverlayImage;
        mWorkItem = nullptr;

        mBase.set(*base);
        mOverlay.set(*mOverlayImage);
    }

    void GlobalMap::asyncWritePng()
    {
        if (mOverlayImage == nullptr)
            return;
        // Use deep copy to avoid any sychronization
        mWritePng = new WritePng(new osg::Image(*mOverlayImage, osg::CopyOp::DEEP_COPY_ALL));
        mWorkQueue->addWorkItem(mWritePng, /*front=*/true);
    }
}

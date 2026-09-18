#include "sharedtexture.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <osg/Image>
#include <osg/Texture2D>

#include <components/rtx/guirenderer.hpp>

#include "rgbarows.hpp"

namespace MyGUIRtx
{
    SharedTexture::SharedTexture(Rtx::GuiRenderer& renderer, osg::Texture2D& source)
        : MirrorTexture(renderer)
        , mSource(&source)
    {
        refresh();
    }

    SharedTexture::~SharedTexture() = default;

    void SharedTexture::refresh()
    {
        const osg::Image* const image = mSource->getImage();
        if (image == nullptr || image->s() <= 0 || image->t() <= 0)
            return;

        if (image == mSeen && image->getModifiedCount() == mSeenCount)
            return;

        // A picture of another shape is another slot: the table sizes a slot once.
        if (getSlot().isNone() || image->s() != getWidth() || image->t() != getHeight())
        {
            take(image->s(), image->t());
            mLastSent.clear();
        }

        // The run of rows that differ from what was sent, against the image's own bytes rather
        // than the widened ones: the comparison is over the picture as the game wrote it, and
        // widening is paid for the rows that go. The same image with a moved count is the fog of
        // war or the world map, written in a corner; another image under the texture is a video
        // frame, and goes whole.
        const int height = getHeight();
        const std::size_t rowBytes = image->getRowSizeInBytes();
        const std::size_t total = rowBytes * static_cast<std::size_t>(height);
        int first = 0;
        int last = height - 1;
        if (image == mSeen && image->isDataContiguous() && mLastSent.size() == total)
        {
            while (
                first <= last && std::memcmp(image->data(0, first), mLastSent.data() + rowBytes * first, rowBytes) == 0)
                ++first;
            while (last > first && std::memcmp(image->data(0, last), mLastSent.data() + rowBytes * last, rowBytes) == 0)
                --last;
        }

        mSeen = image;
        mSeenCount = image->getModifiedCount();

        if (first > last)
            return;

        const std::uint32_t count = static_cast<std::uint32_t>(last - first + 1);
        const Rtx::GuiRegion rows{ 0, static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(getWidth()),
            count };
        writeRgbaRows(*image, first, static_cast<int>(count), mRenderer.lendGuiTexture(getSlot(), rows).data());
        mRenderer.sendGuiTexture(getSlot());

        if (image->isDataContiguous())
        {
            mLastSent.resize(total);
            std::memcpy(mLastSent.data() + rowBytes * first, image->data(0, first), rowBytes * count);
        }
        else
            mLastSent.clear();
    }
}

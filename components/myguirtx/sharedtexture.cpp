#include "sharedtexture.hpp"

#include <osg/Image>
#include <osg/Texture2D>

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

        mSeen = image;
        mSeenCount = image->getModifiedCount();

        // A picture of another shape is another slot: the table sizes a slot once.
        if (getSlot().isNone() || image->s() != getWidth() || image->t() != getHeight())
            take(image->s(), image->t());

        sendImage(*image);
    }
}

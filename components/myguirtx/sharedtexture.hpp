#pragma once

#include <osg/ref_ptr>

#include <components/rtx/guirenderer.hpp>

#include "mirrortexture.hpp"

namespace osg
{
    class Image;
    class Texture2D;
}

namespace MyGUIRtx
{
    /// A mirror of a picture the game holds as an `osg::Image`: a video frame, the world map's
    /// base, a save's thumbnail and the frozen loading frame. This backend cannot draw an OSG
    /// texture, so it reads the image again whenever another one is put under the texture or its
    /// modified count moves — `refresh`, from `doRender` — and sends it whole.
    ///
    /// **Whole, because none of these is written in part.** A video puts each frame under the
    /// texture as another image, and the other three are written once. A picture written in part
    /// names the rectangle it painted, and is `PaintedMirror`: a comparison against a copy of what
    /// was sent would find that rectangle at the price of the copy, which for the frozen frame is
    /// the whole output held for the life of the game.
    class SharedTexture final : public MirrorTexture
    {
    public:
        /// `source` belongs to the caller and outlives this.
        SharedTexture(Rtx::GuiRenderer& renderer, osg::Texture2D& source);

        /// Out of line, so a holder of one needs no more of OpenSceneGraph than a name.
        ~SharedTexture() override;

        /// Brings the mirror up to date with its image, once per draw and not per write, because a
        /// draw is where the picture is read.
        void refresh() override;

    private:
        osg::ref_ptr<osg::Texture2D> mSource;

        /// The image last sent and its modified count, so `refresh` can tell a draw with nothing
        /// new from one with a new picture. Held, because an image the game let go of could be
        /// followed by another at its address with the same count, which would then never be sent.
        osg::ref_ptr<const osg::Image> mSeen;
        unsigned int mSeenCount = 0;
    };
}

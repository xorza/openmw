#pragma once

#include <cstdint>
#include <vector>

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
    /// A mirror of a picture the game holds as an `osg::Image`. The fog of war, the world map, a
    /// save's thumbnail and a video frame are written into images the game then marks dirty, which
    /// is how the rasterizer is handed them; this backend cannot draw an OSG texture, so it reads
    /// the image again whenever its modified count moves — `refresh`, from `doRender` — and sends
    /// the rows that changed. A picture whose painter names what it painted is `PaintedMirror`.
    class SharedTexture final : public MirrorTexture
    {
    public:
        /// `source` belongs to the caller and outlives this.
        SharedTexture(Rtx::GuiRenderer& renderer, osg::Texture2D& source);

        /// Out of line, so a holder of one needs no more of OpenSceneGraph than a name.
        ~SharedTexture() override;

        /// Brings the mirror up to date with its image.
        ///
        /// **Once per draw and not per write**, because the game writes the fog of war a texel at a
        /// time and the interface draws it once. What was seen last is kept, so what goes to the
        /// device is the run of rows that differ — the world map paints eighteen pixels square when a
        /// cell arrives, and the whole picture is two megabytes. A new image under the texture is a
        /// video frame, and goes whole.
        void refresh() override;

    private:
        /// The picture this mirrors.
        osg::ref_ptr<osg::Texture2D> mSource;

        /// The image last sent and its modified count, so `refresh` can tell a frame with nothing
        /// new from one with a row changed.
        const osg::Image* mSeen = nullptr;
        unsigned int mSeenCount = 0;

        /// A copy of the image's own bytes as last sent, row by row, so a write of a few rows costs
        /// a comparison and those rows. Empty until the first send.
        std::vector<std::uint8_t> mLastSent;
    };
}

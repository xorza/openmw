#pragma once

#include <cstdint>

#include <osg/ref_ptr>

#include <components/rtx/guirenderer.hpp>

#include "mirrortexture.hpp"

namespace SceneUtil
{
    class PaintedTexture;
}

namespace MyGUIRtx
{
    /// A mirror of a picture the game paints, `SceneUtil::PaintedTexture`: the fog of war and the
    /// world map's overlay. Where `SharedTexture` compares every row of its image to find what
    /// changed, this is told — the painter names the rectangle — and sends that rectangle and
    /// nothing else.
    class PaintedMirror final : public MirrorTexture
    {
    public:
        /// `source` belongs to the caller and outlives this.
        PaintedMirror(Rtx::GuiRenderer& renderer, SceneUtil::PaintedTexture& source);

        /// Out of line, so a holder of one needs no more of the picture than a name.
        ~PaintedMirror() override;

        /// Sends what was painted since the last draw: the whole picture first, then each
        /// rectangle the painter named. Once per draw, from `doRender`.
        void refresh() override;

    private:
        osg::ref_ptr<SceneUtil::PaintedTexture> mSource;

        /// The paint count this last sent up to, and whether it has sent the picture at all: a
        /// picture painted nowhere yet is still a picture to show.
        std::uint32_t mSeen = 0;
        bool mSent = false;
    };
}

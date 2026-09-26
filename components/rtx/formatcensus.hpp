#pragma once

#include <array>
#include <cstdint>

#include "texturedata.hpp"
#include "textureencoding.hpp"

namespace osg
{
    class Image;
}

namespace Rtx
{
    /// How many textures of one format stand, and how many of those brought mips.
    struct FormatCount
    {
        std::uint32_t mMet = 0;
        std::uint32_t mMipped = 0;
    };

    /// What the textures a scene stands turned out to be, one entry per `TextureFormat`, counted
    /// by enumerator and named at the end, because naming one where it is met builds a
    /// `std::string` on the frame path. Its own struct because the unnamed format is the last one
    /// seen rather than a total.
    struct FormatCensus
    {
        std::array<FormatCount, sTextureFormatCount> mMet{};

        /// The pixel format the `Unnamed` count last stood for, or zero — the whole of what makes
        /// that count worth printing, because a format nothing names is a canary and the reader's
        /// next step is to look this one up.
        std::uint32_t mUnnamed = 0;

        /// Counts `image` under its format as `encoding`, and its mips beside it.
        void count(const osg::Image& image, TextureEncoding encoding);

        /// Takes back what `count` added for the same image and encoding.
        void discount(const osg::Image& image, TextureEncoding encoding);
    };
}

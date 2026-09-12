#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include <osg/Image>
#include <osg/ref_ptr>

#include "namedenum.hpp"
#include "shaders/gbuffer.h"

namespace Rtx
{
    /// One of the images the trace writes and everything after it reads.
    ///
    /// **The shader's own numbering, and not a second list beside it.** The bindings are
    /// `CHANNEL_*` from `shaders/gbuffer.h`, so a value here is the binding — and a channel added
    /// to the shader without a case here is a build failure rather than a read of whichever image
    /// happened to sit at that number.
    enum class Channel : std::uint32_t
    {
        Direct = Shaders::CHANNEL_DIRECT,
        Indirect = Shaders::CHANNEL_INDIRECT,
        Albedo = Shaders::CHANNEL_ALBEDO,
        Specular = Shaders::CHANNEL_SPECULAR,
        Guide = Shaders::CHANNEL_GUIDE,
        Motion = Shaders::CHANNEL_MOTION,
        Depth = Shaders::CHANNEL_DEPTH,
        ReflectionMotion = Shaders::CHANNEL_REFLECTION_MOTION,
        ParticleMask = Shaders::CHANNEL_PARTICLE_MASK,
        BiasMask = Shaders::CHANNEL_BIAS_MASK,
        StarsShown = Shaders::CHANNEL_STARS_SHOWN,
        Transparency = Shaders::CHANNEL_TRANSPARENCY,
        TransparencyOpacity = Shaders::CHANNEL_TRANSPARENCY_OPACITY,
        TransparencyMotion = Shaders::CHANNEL_TRANSPARENCY_MOTION,
    };

    inline constexpr std::uint32_t sChannelCount = Shaders::CHANNEL_COUNT;

    inline constexpr std::uint32_t bindingOf(Channel channel)
    {
        return static_cast<std::uint32_t>(channel);
    }

    /// What a capture and a dump call each channel, and the one place they are written.
    ///
    /// **One table, because this enum was spelled three times**: here, in a list of every value and
    /// in a switch that named them. `Rtx::NamedEnum` derives the walk and the printable list from
    /// the names, so a channel added to the shader cannot reach one of the three and miss another.
    ///
    /// In binding order, which is what `values()` then hands a walk that wants them all.
    inline constexpr NamedEnum<Channel, sChannelCount> sChannels{ { {
        { Channel::Direct, "g-direct" },
        { Channel::Indirect, "g-indirect" },
        { Channel::Albedo, "g-albedo" },
        { Channel::Specular, "g-specular" },
        { Channel::Guide, "g-guide" },
        { Channel::Motion, "g-motion" },
        { Channel::Depth, "g-depth" },
        { Channel::ReflectionMotion, "g-reflection-motion" },
        { Channel::ParticleMask, "g-particle-mask" },
        { Channel::BiasMask, "g-bias-mask" },
        { Channel::StarsShown, "g-stars-shown" },
        { Channel::Transparency, "g-transparency" },
        { Channel::TransparencyOpacity, "g-transparency-opacity" },
        { Channel::TransparencyMotion, "g-transparency-motion" },
    } } };

    /// Every channel in binding order, for a walk that wants them all.
    inline constexpr std::array<Channel, sChannelCount> sEveryChannel = sChannels.values();

    inline constexpr std::string_view channelName(const Channel channel)
    {
        return sChannels.name(channel);
    }

    /// The two images a frame carries that are not channels of the trace's g-buffer.
    ///
    /// **Named apart, because they are not written by the trace and have no binding.** The
    /// composite's output is the frame every channel was gathered to make, and the accumulation is
    /// the wavelet's own blend — which a frame no wavelet reconstructed does not have at all.
    enum class FrameImage
    {
        /// What the composite drew, which is the picture.
        Composite,

        /// What the wavelet blended over the frames before it. Only where one ran.
        Accumulated,
    };

    /// A traced frame as a backend hands it over: tightly packed 8-bit RGBA, row zero at the top.
    struct TracedFrame
    {
        std::uint32_t mWidth = 0;
        std::uint32_t mHeight = 0;
        std::span<const std::uint8_t> mPixels;
    };

    /// Which end of the picture row zero of the result holds.
    enum class RowOrder
    {
        /// The trace's own, and what MyGUI takes: `MyGUIPlatform::Picture` copies an image straight
        /// into a locked texture and the interface draws it from the top down.
        TopFirst,

        /// OpenSceneGraph's, and what `osgDB`'s writers and a savegame thumbnail expect.
        BottomFirst,
    };

    /// How many bytes a pixel of the result carries, which is its pixel format.
    enum class Channels : int
    {
        /// The frame's own, and what a locked texture and a PNG take.
        Rgba = 4,

        /// A savegame thumbnail's: its writer is JPEG, which has no alpha to carry and refuses a
        /// four-channel image outright — an `ERROR_IN_WRITING_FILE` and a save with no picture in
        /// it, which is what the rasterizer avoids by reading its screenshots back as `GL_RGB`.
        Rgb = 3,
    };

    /// The frame as an `osg::Image` of the size and the format asked for, or null where there is
    /// nothing to give.
    ///
    /// **Nearest, and resampled here rather than by `osg::Image::scaleImage`**, which is
    /// `gluScaleImage` — a GL call, and on this path there is no context to make it in. A save asks
    /// for its thumbnail at a hundred pixels across, where a box filter rounds to the same texels.
    ///
    /// @return null where either extent is zero or `frame.mPixels` is shorter than the frame it
    ///         claims to be, because a picture of part of a frame is worse than none.
    osg::ref_ptr<osg::Image> frameImage(
        const TracedFrame& frame, int width, int height, RowOrder order, Channels channels = Channels::Rgba);
}

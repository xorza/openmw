#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <components/rtx/shaders/colour.h>
#include <components/rtx/shaders/look.h>

namespace Rtx::Testing
{
    /// A linear value as the display curve writes it, so a test can name the byte it expects.
    ///
    /// **The display curve and not the whole of what `tone.comp` does**: that pass grades and runs
    /// `toneMap` first, and a test measuring what the trace computed wants the radiance rather
    /// than the picture made of it. A visibility frame's `byte` encodes with this for the same reason.
    inline std::uint8_t encodeSrgb(float linear)
    {
        return static_cast<std::uint8_t>(std::lround(std::clamp(Shaders::encodeSrgb(linear), 0.0f, 1.0f) * 255.0f));
    }

    /// The byte `tone.comp` writes for a grey that reaches it at `linear`, exposure applied, under
    /// whatever the grade's dials in `look.h` say.
    ///
    /// **A grey on the curve's straight stretch alone, because that is the stretch a test can count
    /// by hand**: past three times the shadow offset, so the whole of it comes off, and under the
    /// compression point, so nothing else does. The saturation grade leaves a grey where it is.
    /// Outside the stretch this throws rather than answering with a number nobody derived, and says
    /// which end the dials carried the grey past.
    inline std::uint8_t displayedGrey(float linear)
    {
        const float graded = linear * Shaders::contrastScale(linear, Shaders::TONE_CONTRAST);
        if (graded < 3.0f * Shaders::TONE_SHADOW_OFFSET)
            throw std::runtime_error("a grey of " + std::to_string(linear) + " is graded under the offset's ramp");
        if (!(graded - Shaders::TONE_SHADOW_OFFSET < Shaders::TONE_COMPRESSION_START))
            throw std::runtime_error("a grey of " + std::to_string(linear) + " is graded into the compression");

        return encodeSrgb(graded - Shaders::TONE_SHADOW_OFFSET);
    }
}

#pragma once

#include <cstdint>
#include <string>

#include <osg/Vec3f>

namespace Rtx
{
    struct ValidationOptions;
}

namespace RtxTool
{
    struct ViewRequest;

    /// Where the camera is standing and under what, as the window's own prints take it.
    ///
    /// **A type rather than a handful of `format` calls in the key handler**, because everything it
    /// writes is something the tool has to be able to read back — a `views.cfg` section and a
    /// command line — and a format that drifts from its parser is not a thing an eye catches in a
    /// log. Held apart from the window so the tests can assert both without a device.
    struct Viewpoint
    {
        /// The `views.cfg` id this was opened as, or empty where it was opened by `--cell`. Kept so
        /// that flying somewhere better and saving it is a replacement rather than a new entry.
        std::string mView;
        std::string mNote;

        /// The cell as `--cell` spells it: a pair of integers for an exterior, a name for an
        /// interior.
        ///
        /// **The cell the window opened, and not the one the camera has since flown into.** The
        /// reference implementation prints the containing square instead, and is right to: it
        /// streams, so its camera really is standing in the cell it names. This tool loads exactly
        /// one cell and shows nothing outside it, so a marker naming the square the camera drifted
        /// into would reload a different cell and render a different frame — which is the opposite
        /// of what a marker is for.
        std::string mCell;

        osg::Vec3f mOrigin;
        osg::Vec3f mTarget;

        /// As the fallback settings spell it, and a twenty-four hour clock.
        std::string mWeather;
        float mHour = 12.0f;

        /// Which day, counted from the one a new game begins on. Only the moons read it.
        int mDay = 0;

        /// Degrees clockwise from north, in `[0, 360)`.
        ///
        /// **North is +Y and east is +X**, so the arguments come the other way round from the usual
        /// `atan2`.
        float getBearing() const;

        /// Degrees above the horizon, in `[-90, 90]`.
        float getClimb() const;
    };

    /// The hour as a clock reads it. Rounded to the minute, which is finer than the sun moves.
    std::string clockFace(float hour);

    /// One line for a person: where this is, in numbers worth reading rather than round-tripping.
    ///
    /// A `#` comment in both of the formats below, so a file of these can be fed to either.
    std::string describeSpot(const Viewpoint& spot);

    /// The whole `views.cfg` section, ready to paste into it.
    ///
    /// **The whole section and not two of its lines.** A block with no `cell` in it is one the view
    /// file refuses to load, so what the window printed could never have gone where it was printed
    /// to go. Numbers are shortest-round-trip for the reason `describeProfile` gives.
    std::string describeBlock(const Viewpoint& spot);

    /// One line of arguments that renders this frame again, wherever it is pasted.
    ///
    /// **What the window is looking at, plus everything that changes what it costs.** The camera and
    /// the size are passed rather than read off `request` because both move while the window is
    /// open; the rest of the conditions do not, and come off the request as they were given.
    ///
    /// The denoiser and the validation flags are in it deliberately, because both cost time a
    /// profiling line has to account for: five wavelet levels are about 2 ms at 1080p, and a trace
    /// timed under the layers is not a figure to compare against anything at all.
    std::string describeProfile(const ViewRequest& request, const Rtx::ValidationOptions& validation,
        const osg::Vec3f& origin, const osg::Vec3f& target, std::uint32_t width, std::uint32_t height);
}

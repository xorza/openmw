#pragma once

#include <string>
#include <string_view>

#include <osg/Vec3f>

#include "views.hpp"

namespace RtxTool
{
    /// Where a camera is standing and under what, as this tool writes a place down.
    ///
    /// **How a place found by flying is written into `views.cfg`.** A run that opened a window
    /// prints one of these where the eye was left, so somebody who flew somewhere worth keeping
    /// closes the window and pastes what it said.
    ///
    /// **A type rather than a handful of `format` calls at whatever prints it**, because everything
    /// it writes is something the tool has to be able to read back — a `views.cfg` section and a
    /// command line — and a format that drifts from its parser is not a thing an eye catches in a
    /// log. Held apart from whatever prints it, so the tests can assert both without a device.
    struct Viewpoint
    {
        /// The `views.cfg` id this was opened as, or empty where it was opened by `--cell`. Kept so
        /// that flying somewhere better and saving it is a replacement rather than a new entry.
        std::string mView;
        std::string mNote;

        /// The cell as `--cell` spells it: a pair of integers for an exterior, a name for an
        /// interior.
        ///
        /// **Where a run enters, and not the square a camera has since flown into.** The world is
        /// streamed around the player, and a stop moves the player to `mOrigin` once it is standing
        /// somewhere — so the two disagreeing costs nothing, and the block still opens on the frame
        /// it was printed from. Naming the containing square would ask the game to spell a cell
        /// back, which is a second spelling of a name `--cell` already reads.
        std::string mCell;

        osg::Vec3f mOrigin;
        osg::Vec3f mTarget;

        /// As the fallback settings spell it, and a twenty-four hour clock.
        ///
        /// **The same defaults the view file's absent `weather` and `hour` mean**, because
        /// `describeBlock` writes each out only where it differs from one — a disagreement would put
        /// `hour = 12` into every pasted block and fix at clear noon a place that meant to take the
        /// run's own conditions.
        std::string mWeather = std::string(sDefaultWeather);
        float mHour = sDefaultHour;

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

    /// One line for a person: where this is, in numbers worth reading rather than round-tripping.
    ///
    /// A `#` comment in both of the formats below, so a file of these can be fed to either.
    std::string describeSpot(const Viewpoint& spot);

    /// The whole `views.cfg` section, ready to paste into it.
    ///
    /// **The whole section and not two of its lines.** A block with no `cell` in it is one the view
    /// file refuses to load, so what was printed could never have gone where it was printed to go.
    ///
    /// **Shortest-round-trip numbers and not the rounded ones `describeSpot` prints**: these exist
    /// to be read back into the same floats, and a position rounded to the unit is a different
    /// frame when the camera is a hand's width from a wall.
    std::string describeBlock(const Viewpoint& spot);
}

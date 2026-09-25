#include "benchrun.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <string_view>

#include <components/rtx/reconstruction.hpp>

namespace Rtx
{
    namespace
    {
        /// One check: the word it is asked for by, and whether a stop is shaped to answer it.
        struct CheckRow
        {
            Check mCheck;
            std::string_view mName;

            /// A claim a stop is not shaped for answers something else, so it is left out rather
            /// than counted as a failure: a crossing count needs a route to cross anything with,
            /// and only the view says whether there is one.
            bool (*mCanAsk)(const Stop& stop, const RenderProfile& profile);
        };

        constexpr bool always(const Stop&, const RenderProfile&)
        {
            return true;
        }

        constexpr bool routed(const Stop& stop, const RenderProfile&)
        {
            return stop.mSchedule.mRoute.has_value();
        }

        /// A route arrives at cells, and an arrival that rebuilds the scene, or places it twice in
        /// one frame, drains the ring.
        constexpr bool unrouted(const Stop& stop, const RenderProfile&)
        {
            return !stop.mSchedule.mRoute.has_value();
        }

        /// **Asked of a stop that stands still.** A route leaves the camera wherever it flew to and
        /// `Stand` names only where it set off from, so the two legitimately differ by the whole
        /// length of the route.
        constexpr bool standingStill(const Stop& stop, const RenderProfile&)
        {
            return stop.mStand.mEye.has_value() && !stop.mSchedule.mFreeCamera && !stop.mSchedule.mRoute.has_value();
        }

        /// A hold a run did not ask for cannot come out short.
        constexpr bool held(const Stop&, const RenderProfile& profile)
        {
            return profile.mStressOverlapMs > 0.0;
        }

        /// Every check, the word it is asked for by, and when it may be asked.
        ///
        /// **The one list**, so a check renamed here is renamed in the command line and in the
        /// report at once, and a check added here reaches the runner and says what it may be asked
        /// of without anybody remembering to list it a second time. What a check answers is
        /// `StopWriter::checkHolds`, which needs the game and so stays in the harness.
        constexpr std::array sChecks{
            CheckRow{ Check::WalkTwice, "walk-twice", always },
            CheckRow{ Check::SurfacesDescribed, "surfaces-described", always },
            CheckRow{ Check::LightsPlaced, "lights-placed", always },
            CheckRow{ Check::GroundReaches, "ground-reaches", always },
            CheckRow{ Check::GroundStands, "ground-stands", always },
            CheckRow{ Check::LightsNotDoubled, "lights-not-doubled", always },
            CheckRow{ Check::StaticsNotDoubled, "statics-not-doubled", always },
            CheckRow{ Check::TexturesReadable, "textures-readable", always },
            CheckRow{ Check::CameraStands, "camera-stands", standingStill },
            CheckRow{ Check::CrossingsAppend, "crossings-append", routed },
            CheckRow{ Check::FramesOverlap, "frames-overlap", unrouted },
            CheckRow{ Check::QueueHeld, "queue-held", held },
            CheckRow{ Check::Finite, "finite", always },
        };

        /// Whether every enumerator up to the table's length has exactly one row. With
        /// `StopWriter::checkHolds`'s switch, which names every enumerator and no default, a check
        /// added to the enum stops the build until it has a row here and an answer there.
        consteval bool checksComplete()
        {
            for (std::size_t value = 0; value < sChecks.size(); ++value)
            {
                std::size_t rows = 0;
                for (const CheckRow& row : sChecks)
                    rows += static_cast<std::size_t>(row.mCheck) == value ? 1 : 0;
                if (rows != 1)
                    return false;
            }

            return true;
        }

        static_assert(checksComplete(), "every Check has one row in sChecks, and the rows are the enumerators");

        constexpr std::array<Check, sChecks.size()> sEvery = [] {
            std::array<Check, sChecks.size()> every{};
            for (std::size_t at = 0; at < sChecks.size(); ++at)
                every[at] = sChecks[at].mCheck;
            return every;
        }();

        const CheckRow& rowOf(const Check check)
        {
            const auto found = std::find_if(
                sChecks.begin(), sChecks.end(), [check](const CheckRow& row) { return row.mCheck == check; });
            assert(found != sChecks.end() && "a check with no row: checksComplete holds this shut");
            return *found;
        }
    }

    std::string_view checkName(const Check check)
    {
        return rowOf(check).mName;
    }

    std::span<const Check> everyCheck()
    {
        return sEvery;
    }

    bool canAsk(const Check check, const Stop& stop, const RenderProfile& profile)
    {
        return rowOf(check).mCanAsk(stop, profile);
    }

    osg::Vec3f Stand::getLook() const
    {
        assert(mEye.has_value());

        if (!mLook.has_value() || (*mLook - *mEye).length2() <= 0.0f)
            return *mEye + osg::Vec3f(0.0f, 1.0f, 0.0f);

        return *mLook;
    }

    osg::Vec3f Stand::getRotation() const
    {
        osg::Vec3f forward = getLook() - *mEye;
        forward.normalize();

        // Clamped because a normalised vector's z can land a bit past one, and `asin` answers a NaN
        // rather than a right angle when it does.
        return osg::Vec3f(-std::asin(std::clamp(forward.z(), -1.0f, 1.0f)), 0.0f, std::atan2(forward.x(), forward.y()));
    }

    osg::Vec3f Stand::forwardOf(const osg::Vec3f& rotation)
    {
        const float level = std::cos(rotation.x());
        return osg::Vec3f(std::sin(rotation.z()) * level, std::cos(rotation.z()) * level, -std::sin(rotation.x()));
    }
}

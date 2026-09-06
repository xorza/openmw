#include "viewpoint.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include <osg/Math>

#include <components/rtxbench/benchrecord.hpp>

#include "views.hpp"

namespace RtxTool
{
    namespace
    {
        /// A view id derived from a cell's name, for a window that was opened without one.
        ///
        /// Something to paste rather than something to keep: the ids in the file are chosen to say
        /// what a view is *for*, which a cell name cannot.
        std::string slugOf(std::string_view cell)
        {
            std::string slug;
            for (const char letter : cell)
            {
                const bool plain = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z')
                    || (letter >= '0' && letter <= '9');
                if (plain)
                    slug += static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
                else if (!slug.empty() && slug.back() != '-')
                    slug += '-';
            }

            while (!slug.empty() && slug.back() == '-')
                slug.pop_back();

            return slug.empty() ? "new-view" : slug;
        }

    }

    float Viewpoint::getBearing() const
    {
        osg::Vec3f forward = mTarget - mOrigin;
        forward.normalize();

        const float degrees = osg::RadiansToDegrees(std::atan2(forward.x(), forward.y()));
        return degrees < 0.0f ? degrees + 360.0f : degrees;
    }

    float Viewpoint::getClimb() const
    {
        osg::Vec3f forward = mTarget - mOrigin;
        forward.normalize();

        // Clamped because a normalised vector's z can land a bit past one, and `asin` answers a NaN
        // rather than ninety degrees when it does.
        return osg::RadiansToDegrees(std::asin(std::clamp(forward.z(), -1.0f, 1.0f)));
    }

    std::string describeSpot(const Viewpoint& spot)
    {
        return std::format("# {} at {:.0f}, {:.0f}, {:.0f} — bearing {:.0f}°, climb {:.0f}° — day {}, {}, {}\n",
            spot.mCell, spot.mOrigin.x(), spot.mOrigin.y(), spot.mOrigin.z(), spot.getBearing(), spot.getClimb(),
            spot.mDay, Rtx::describeHour(spot.mHour), spot.mWeather);
    }

    std::string describeBlock(const Viewpoint& spot)
    {
        std::string block = std::format("[{}]\n", spot.mView.empty() ? slugOf(spot.mCell) : spot.mView);

        if (!spot.mNote.empty())
            block += std::format("note = {}\n", spot.mNote);

        block += std::format("cell = {}\npos = {}, {}, {}\nlook = {}, {}, {}\n", spot.mCell, spot.mOrigin.x(),
            spot.mOrigin.y(), spot.mOrigin.z(), spot.mTarget.x(), spot.mTarget.y(), spot.mTarget.z());

        // **Each condition only where the window was not at the file's own**, because one written
        // down fixes the place under it. A block pasted from a window flown at dawn in a storm has
        // to bring both with it — the light is most of what the frame is — and one from a window at
        // clear noon should leave the view free to be measured under whatever a run names.
        if (spot.mHour != sDefaultHour)
            block += std::format("hour = {}\n", spot.mHour);

        if (spot.mWeather != sDefaultWeather)
            block += std::format("weather = {}\n", spot.mWeather);

        return block;
    }
}

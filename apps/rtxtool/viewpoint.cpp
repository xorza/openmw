#include "viewpoint.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include <osg/Math>

#include <components/rtx/renderer.hpp>

#include "view.hpp"

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

    std::string clockFace(float hour)
    {
        const int minutes = static_cast<int>(std::lround(hour * 60.0f)) % (24 * 60);
        return std::format("{:02}:{:02}", minutes / 60, minutes % 60);
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
            spot.mDay, clockFace(spot.mHour), spot.mWeather);
    }

    std::string describeBlock(const Viewpoint& spot)
    {
        std::string block = std::format("[{}]\n", spot.mView.empty() ? slugOf(spot.mCell) : spot.mView);

        if (!spot.mNote.empty())
            block += std::format("note = {}\n", spot.mNote);

        return block
            + std::format("cell = {}\npos = {}, {}, {}\nlook = {}, {}, {}\n", spot.mCell, spot.mOrigin.x(),
                spot.mOrigin.y(), spot.mOrigin.z(), spot.mTarget.x(), spot.mTarget.y(), spot.mTarget.z());
    }

    std::string describeProfile(const ViewRequest& request, const Rtx::ValidationOptions& validation,
        const osg::Vec3f& origin, const osg::Vec3f& target, std::uint32_t width, std::uint32_t height)
    {
        // Shortest round-trip rather than the rounded form `describeSpot` uses: these numbers exist
        // to be read back into the same floats, and a position rounded to the unit is a different
        // frame when the camera is a hand's width from a wall.
        //
        // The cell is the only field quoted, because it is the only one that can hold a space.
        // A measured exposure is a different frame *and* a different cost from a held one, which is
        // both reasons a field is in this line.
        //
        // **The reconstruction is named for the same two reasons, and it was the omission that made
        // this line not reproduce a frame.** `shot` upscales at quality unless told otherwise, so a
        // window flown at another mode profiled into a command that rendered something else; and
        // the network is a picture and a cost that the installed library was free to change out
        // from under a corpus that never said which one it meant.
        const std::string exposure
            = request.mExposure.has_value() ? std::format("{}", *request.mExposure) : std::string("auto");

        return std::format(
            "--cell=\"{}\" --pos={},{},{} --look={},{},{} --fov={} --size={}x{} --weather={}"
            " --hour={} --day={} --exposure={} --upscale={} --preset={} --filter={} --validation={}"
            " --sync-validation={} --gpu-validation={}{}",
            request.mCell, origin.x(), origin.y(), origin.z(), target.x(), target.y(), target.z(), request.mFieldOfView,
            width, height, request.mWeather, request.mHour, request.mDay, exposure, Rtx::upscaleName(request.mUpscale),
            Rtx::presetName(request.mPreset), request.mFilter, validation.mEnabled, validation.mSynchronization,
            validation.mGpuAssisted, request.mShowAlbedo ? " --albedo" : "");
    }
}

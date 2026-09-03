#include "viewpoint.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include <osg/Math>

#include <components/rtx/renderer.hpp>

#include "view.hpp"
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

    std::string describeTitle(const WindowTitle& title)
    {
        // Both extents only where they differ, which is every run that upscales and no other.
        std::string sizes = std::format("{}x{}", title.mOutputWidth, title.mOutputHeight);
        if (title.mRenderWidth != title.mOutputWidth)
            sizes = std::format("{}x{} to {}", title.mRenderWidth, title.mRenderHeight, sizes);

        const std::string sky = title.mInto.empty()
            ? std::string(title.mWeather)
            : std::format("{} to {} {:.0f}%", title.mWeather, title.mInto, title.mTurned * 100.0f);

        return std::format("{}  |  {:.0f} fps  |  {}  |  {:.0f}, {:.0f}, {:.0f}  |  {:.0f} u/s  |  day {} {} {}",
            title.mName, title.mFps, sizes, title.mOrigin.x(), title.mOrigin.y(), title.mOrigin.z(), title.mSpeed,
            title.mDay, clockFace(title.mHour), sky);
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
        //
        // **The reorder is named for both of those reasons as well.** It moves the trace by 7 to 17
        // percent and it moves a scattering of pixels, so a line without it says one thing about two
        // frames that cost different amounts.
        const std::string exposure
            = request.mFrame.mExposure.has_value() ? std::format("{}", *request.mFrame.mExposure) : std::string("auto");

        return std::format(
            "--cell=\"{}\" --pos={},{},{} --look={},{},{} --fov={} --size={}x{} --weather={}"
            " --hour={} --day={} --exposure={} --upscale={} --preset={} --reorder={} --filter={}"
            " --validation={} --sync-validation={} --gpu-validation={}{}",
            request.mCell, origin.x(), origin.y(), origin.z(), target.x(), target.y(), target.z(),
            request.mFrame.mFieldOfView, width, height, request.mFrame.mWeather, request.mFrame.mHour,
            request.mFrame.mDay, exposure, Rtx::upscaleName(request.mFrame.mUpscale),
            Rtx::presetName(request.mFrame.mPreset), Rtx::reorderName(request.mFrame.mReorder), request.mFrame.mFilter,
            validation.mEnabled, validation.mSynchronization, validation.mGpuAssisted,
            request.mShowAlbedo ? " --albedo" : "");
    }
}

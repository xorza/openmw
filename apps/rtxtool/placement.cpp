#include "placement.hpp"

#include <cmath>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

#include <osg/Math>

#include <components/esm/position.hpp>

namespace RtxTool
{
    Placement placeCamera(const osg::BoundingBoxf& bounds, float verticalFovDegrees,
        const std::optional<osg::Vec3f>& origin, const std::optional<osg::Vec3f>& target)
    {
        const osg::Vec3f centre = bounds.center();

        osg::Vec3f direction(0.6f, 0.6f, 0.35f);
        direction.normalize();

        // Far enough back that the bounding sphere fits the vertical field of view, and a little
        // further so it is not touching the edges.
        const float distance = bounds.radius() / std::tan(osg::DegreesToRadians(verticalFovDegrees) * 0.5f) * 1.15f;

        return Placement{
            .mOrigin = origin.value_or(centre + direction * distance),
            .mTarget = target.value_or(centre),
        };
    }

    Placement placeOnArrival(
        const ESM::Position& arrival, const std::optional<osg::Vec3f>& origin, const std::optional<osg::Vec3f>& target)
    {
        // About how high a Dunmer's eyes are above the floor they stand on. The arrival is where the
        // feet go.
        constexpr float sEye = 80.0f;

        // How far down the heading the camera looks. Past the far wall of most rooms, which is what
        // a direction wants: the point is where the eye is aimed, not what it stops at.
        constexpr float sAlong = 2048.0f;

        const osg::Vec3f eye(arrival.pos[0], arrival.pos[1], arrival.pos[2] + sEye);

        // **The heading the game would give the player**, which is a Z rotation and measured the way
        // the engine measures it: clockwise from north rather than counter-clockwise from east.
        const float heading = arrival.rot[2];
        const osg::Vec3f ahead(std::sin(heading), std::cos(heading), 0.0f);

        return Placement{
            .mOrigin = origin.value_or(eye),
            .mTarget = target.value_or(eye + ahead * sAlong),
        };
    }

    std::optional<osg::Vec3f> parseVec3(std::string_view text, std::string_view what)
    {
        if (text.empty())
            return std::nullopt;

        const auto fail = [&] {
            throw std::runtime_error(
                std::string(what) + " is not three numbers separated by commas: \"" + std::string(text) + '"');
        };

        osg::Vec3f result;
        for (int axis = 0; axis < 3; ++axis)
        {
            while (!text.empty() && text.front() == ' ')
                text.remove_prefix(1);

            const std::size_t comma = text.find(',');
            const std::string_view field = text.substr(0, comma);

            // Not `std::from_chars`: libc++ ships the floating-point overload only from macOS 26,
            // and this is a command line, where trailing rubbish has to be rejected rather than
            // quietly ignored. The classic locale is what keeps the decimal separator a point
            // wherever this runs, and `eof` is what says the whole field was consumed — the same
            // question `from_chars` answers with its end pointer.
            std::istringstream stream{ std::string(field) };
            stream.imbue(std::locale::classic());
            if (!(stream >> result[axis]) || !stream.eof())
                fail();

            const bool last = axis == 2;
            if ((comma == std::string_view::npos) != last)
                fail();

            if (!last)
                text = text.substr(comma + 1);
        }

        return result;
    }
}

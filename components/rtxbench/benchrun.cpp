#include "benchrun.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <string_view>
#include <utility>

#include <components/rtx/namedenum.hpp>

namespace Rtx
{
    namespace
    {
        /// Every check and the word it is asked for by.
        ///
        /// **The one list of the names**, so a check renamed here is renamed in the command line
        /// and in the report at once, and a check added here reaches the runner without anybody
        /// remembering to list it a second time.
        constexpr Rtx::NamedEnum sChecks{ std::array{
            std::pair{ Check::WalkTwice, std::string_view("walk-twice") },
            std::pair{ Check::SurfacesDescribed, std::string_view("surfaces-described") },
            std::pair{ Check::LightsPlaced, std::string_view("lights-placed") },
            std::pair{ Check::GroundReaches, std::string_view("ground-reaches") },
            std::pair{ Check::GroundStands, std::string_view("ground-stands") },
            std::pair{ Check::LightsNotDoubled, std::string_view("lights-not-doubled") },
            std::pair{ Check::TexturesReadable, std::string_view("textures-readable") },
            std::pair{ Check::CameraStands, std::string_view("camera-stands") },
            std::pair{ Check::CrossingsAppend, std::string_view("crossings-append") },
            std::pair{ Check::FramesOverlap, std::string_view("frames-overlap") },
        } };

        constexpr auto sEvery = sChecks.values();
    }

    std::string_view checkName(const Check check)
    {
        return sChecks.name(check);
    }

    std::span<const Check> everyCheck()
    {
        return sEvery;
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

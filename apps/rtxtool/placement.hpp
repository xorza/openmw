#pragma once

#include <optional>
#include <string_view>

#include <osg/BoundingBox>
#include <osg/Vec3f>

namespace ESM
{
    struct Position;
}

namespace RtxTool
{
    /// Where the camera ends up once whatever was left unsaid has been filled in.
    struct Placement
    {
        osg::Vec3f mOrigin;
        osg::Vec3f mTarget;
    };

    /// Fills in whichever of origin and target was not given, with a view of the whole scene from
    /// outside it.
    ///
    /// Shared by the window and the screenshot so that asking for a picture of what you are looking
    /// at gives you a picture of what you are looking at. It is the one placement that needs nothing
    /// known about the cell — and a poor view of an interior, whose walls stand between the camera
    /// and everything worth seeing. `placeOnArrival` is what an interior something leads to uses.
    Placement placeCamera(const osg::BoundingBoxf& bounds, float verticalFovDegrees,
        const std::optional<osg::Vec3f>& origin, const std::optional<osg::Vec3f>& target);

    /// The same, standing where the game would stand a character who had just walked in.
    ///
    /// **The arrival a door names, facing the way it points.** An interior framed from outside its
    /// own bounds puts the eye through a wall; aiming at the middle of the bounding box points a
    /// winding cave's camera into rock. Where the content actually says to stand is the destination
    /// carried by the door that leads here, and it comes with the heading the player arrives on.
    ///
    /// `arrival` is the `ESM::Position` a teleporting reference carries: the same position and
    /// rotation the game gives the player.
    Placement placeOnArrival(
        const ESM::Position& arrival, const std::optional<osg::Vec3f>& origin, const std::optional<osg::Vec3f>& target);

    /// Parses `x,y,z`. Empty text is not a failure; it means nothing was said.
    ///
    /// Throws `std::runtime_error` naming `what` when the text is present and malformed.
    std::optional<osg::Vec3f> parseVec3(std::string_view text, std::string_view what);
}

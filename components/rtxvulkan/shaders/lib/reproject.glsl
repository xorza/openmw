// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `components/rtx/shaders/portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_REPROJECT_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_REPROJECT_GLSL

// Where everything in the frame stood on the previous frame's screen.
//
// **Four answers, because four things move differently.** A surface moves with the eye and
// with itself; a sprite moves with its own travel; what water reflects moves as its image
// in the plane; and the sky, being infinitely far, moves only when the eye turns.

#include "bindings.glsl"
#include "sprites.glsl"
#include "water.glsl"

/// How far a point moved between the last frame and this one, in world units.
///
/// **A delta and not the previous position**, which is what keeps this honest at Morrowind's
/// distances: the two positions are six figures long and nearly equal, so subtracting them on the
/// device is the mistake the camera path avoids for the same reason. The matrix is exactly the
/// identity for anything that did not move, so `motion * p - p` is bit-exactly zero and a static
/// world produces no motion at all rather than a drift of rounding.
vec3 movedBy(uint index, vec3 position)
{
    const vec4 rows[3] = instanceAt(index).mMotion;
    const vec3 was = vec3(dot(rows[0], vec4(position, 1.0)), dot(rows[1], vec4(position, 1.0)),
        dot(rows[2], vec4(position, 1.0)));

    return was - position;
}

/// Where a point standing `was` from the previous eye lands on that eye's screen, from nought to one
/// across it — or a negative coordinate where it lands on no screen at all.
///
/// **The inverse of the generation in `rayAt`, and shared for the reason `rayAt` itself is.** A
/// pixel reprojects the surface it found and the fog volume reprojects every froxel of its own grid;
/// two derivations of one projection are two chances to disagree about where the previous frame was.
///
/// **An offset from the eye and never a world position.** `mCameraMotion` is the step between two
/// eyes, differenced on the host where a float still has digits to spare — where the two positions
/// themselves are six figures long and nearly equal.
vec2 previousScreen(vec3 was)
{
    // A basis of nothing is what the frame carries where there is no previous frame at all: the
    // first, a resize, a new scene, and any jump a motion vector could not describe.
    if (!(dot(frame.mPreviousForward, frame.mPreviousForward) > 0.0))
        return vec2(-1.0);

    // Behind the previous eye there is no answer, and the divide below would fold such a point back
    // into the frame as a plausible coordinate.
    const float ahead = dot(was, frame.mPreviousForward);
    if (!(ahead > 0.0))
        return vec2(-1.0);

    // The basis carries the image plane's half extents, so dividing by each vector's own square
    // undoes the direction and the scale together.
    const float across = dot(was, frame.mPreviousRight) / dot(frame.mPreviousRight, frame.mPreviousRight);
    const float down = -dot(was, frame.mPreviousUp) / dot(frame.mPreviousUp, frame.mPreviousUp);

    return (vec2(across, down) / ahead) * 0.5 + 0.5;
}

/// Where a surface stood on the previous frame's screen, less where it stands on this one, in
/// pixels.
///
/// **Reprojected as an offset and never as a world point.** `direction * distance` is where the
/// surface is relative to *this* eye, and `mCameraMotion` carries it to where it is relative to the
/// last one — so the only large numbers involved were subtracted on the host, between two camera
/// positions a step apart, where a float is exact.
///
/// **Both ends are where the surface itself projects**, and the near end is therefore the pixel
/// centre plus this frame's jitter — that offset is where the ray that found the surface was aimed,
/// so it is where the surface genuinely lands on this frame's screen. Comparing against the bare
/// centre instead returns the jitter for a world that did not move, and an upscaler handed that
/// fetches its history a fraction of a pixel out, by a different fraction every frame. That is a
/// still image that shakes.
vec2 reprojected(uvec2 pixel, vec3 was)
{
    // **No answer under a parallel projection.** The inverse below divides by the distance along
    // the view axis, which is the perspective divide and not this camera's projection. Nothing that
    // traces one reprojects: a map tile is one frame with no frame before it.
    if (frame.mCamera.mOrthographic != 0u)
        return vec2(0.0);

    const vec2 screen = previousScreen(was);
    if (screen.x < 0.0)
        return vec2(0.0);

    const vec2 before = screen * vec2(frame.mCamera.mWidth, frame.mCamera.mHeight);
    return before - (vec2(pixel) + 0.5 + frame.mCamera.mJitter);
}

vec2 motionOf(uvec2 pixel, vec3 origin, vec3 direction, float distance, uint instance)
{
    const vec3 point = origin + direction * distance;

    return reprojected(pixel, direction * distance + frame.mCameraMotion + movedBy(instance, point));
}

/// Where the puff that owns a pixel stood on the previous frame's screen, in pixels.
///
/// The same reprojection a surface gets: the eye's own walk, and the puff's travel against it. A
/// particle born this frame carries no travel, and neither does a cloud that stands still — which is
/// the truth in both cases, one having no past to point at and the other no motion.
vec2 puffMotionOf(uvec2 pixel, PuffClaim claim)
{
    return reprojected(pixel, claim.mToward + frame.mCameraMotion - claim.mMoved);
}

/// Where what a water surface reflects stood on the previous frame's screen, in pixels.
///
/// **A reflection is not where the water is, and reprojecting it as though it were is what makes a
/// mirrored shoreline swim.** The eye sees the reflected point at its image in the plane, so the
/// thing that has to be reprojected is that image: mirror it about the water level, and mirror its
/// motion with it.
///
/// **Exact for a flat surface and an approximation by exactly the wave.** The image of a point in a
/// tilted facet is not the image in the plane, so this is the reflection the water would carry if it
/// were still — which is what a mirrored reprojection can describe, and is why the wave's own
/// scatter is left to the mask beside it.
///
/// The plane is `frame.mWaterLevel`, which every frame with water in it names: the surface this
/// reflects about is the one the water geometry lies in, and not the facet the ray happened to
/// bounce off.
vec2 mirrorMotionOf(uvec2 pixel, vec3 origin, WaterMirror mirror)
{
    // **The sky reflected is still a reflection that moves**, and writing nought for it says the
    // opposite. It has no distance, so the eye's own walk does not carry it and its turn is the
    // whole of it — the argument `skyMotionOf` makes, about a direction that is mirrored because
    // what is being watched is the image and not the sky.
    if (!mirror.mFound)
        return reprojected(pixel, vec3(mirror.mAlong.xy, -mirror.mAlong.z));

    // The plane's own reflection, which is linear on differences: the constant cancels in the
    // subtraction below, so only `z` changes sign.
    const vec3 seen = vec3(mirror.mAt.xy, 2.0 * frame.mWaterLevel - mirror.mAt.z);
    const vec3 went = movedBy(mirror.mInstance, mirror.mAt);

    return reprojected(pixel, seen - origin + frame.mCameraMotion + vec3(went.xy, -went.z));
}

/// Where the sky a ray found stood on the previous frame's screen, in pixels.
///
/// **Infinitely far, so the eye's own walk does not carry it and its turn is the whole of it.** A
/// miss used to store nothing here, on the reasoning that the sky does not move — which is true of
/// walking and false of looking around, and looking around is most of what a player does. What an
/// upscaler did with it was fetch the sky's history from the pixel it already occupies, so every
/// turn of the head smeared it; a gradient hides that and a field of stars does not.
///
/// The same reprojection a surface gets, with the translation left out: at infinity `mCameraMotion`
/// is nothing beside the direction, and dropping it is what says so exactly rather than nearly.
vec2 skyMotionOf(uvec2 pixel, vec3 direction)
{
    return reprojected(pixel, direction);
}

/// The depth a rasterizer would have written for a hit `along` units down `direction`.
///
/// **Along the view axis and not along the ray.** A rasterizer's depth is the distance to the plane
/// through the eye that faces the way the camera does, so a surface at the corner of the frame is
/// nearer in depth than its distance says — and an upscaler comparing this against its own
/// reprojection would find every corner disagreeing.
///
/// `far / (far - near) * (1 - near / z)`, which is zero at the near plane and one at the far one.
/// A miss writes one: nothing is further away than the end of the world.
float clipDepth(vec3 direction, float along)
{
    const float z = dot(direction, frame.mCamera.mForward) * along;

    // A parallel projection's depth is linear in that distance; it is the perspective divide that
    // makes the expression below the shape it is.
    if (frame.mCamera.mOrthographic != 0u)
        return clamp((z - frame.mNear) / (frame.mFar - frame.mNear), 0.0, 1.0);

    if (!(z > frame.mNear))
        return 0.0;

    return frame.mFar / (frame.mFar - frame.mNear) * (1.0 - frame.mNear / z);
}

#endif

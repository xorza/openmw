// `#pragma once` everywhere else in this tree, and an include guard here for the reason
// `portable.h` gives.
#ifndef OPENMW_COMPONENTS_RTX_SHADERS_CAMERA_H
#define OPENMW_COMPONENTS_RTX_SHADERS_CAMERA_H

#include "portable.h"

// How a pixel becomes a ray, and nothing else about the frame.

#ifdef RTX_HOST

#include <cstdint>

#include <osg/Vec2f>
#include <osg/Vec3f>

namespace Rtx::Shaders
{
    using vec2 = osg::Vec2f;
    using vec3 = osg::Vec3f;
    using uint = std::uint32_t;

#endif

    /// Everything needed to turn a pixel into a ray — **and not where the eye is.**
    ///
    /// **The origin is deliberately absent, and that absence is what makes this shared.** The trace
    /// needs the eye's place in the world and the wavelet does not: every use it makes of a
    /// reconstructed position is a *difference* between two of them, and the origin cancels in a
    /// subtraction. Carrying it here would put twelve bytes of a world coordinate into a push
    /// constant that has no use for one, and would invite a filter to reconstruct absolute positions
    /// at Morrowind's six-figure distances, where a float has nothing left to say. `mNear` and
    /// `mFar` are out for the same reason: they are what a depth is written against, which is the
    /// trace's business alone.
    ///
    /// **Shared because two shaders build the same rays and one of them must build them exactly.**
    /// The wavelet's edge tests compare world positions reconstructed from the guide's distance, and
    /// a position reconstructed from a ray that differs from the one that was shaded — by the
    /// jitter, by the projection, by half a pixel — is not the position of the surface it is
    /// filtering. The two used to be two copies of one derivation, and this struct is the evidence
    /// that they were: every field here was in `AtrousConstants` already, copied across from
    /// `VisibilityConstants` a frame at a time.
    struct Camera
    {
        /// `mRight` and `mUp` are already scaled by the half-extents of the image plane at unit
        /// distance, so a ray is `mForward + mRight * x - mUp * y` for `x` and `y` in [-1, 1] and no
        /// trigonometry in the shader. **`y` runs down the image**, because it is the pixel index
        /// the jitter is added to, which is why it is subtracted: `mUp` points the other way.
        vec3 mForward;
        vec3 mRight;
        vec3 mUp;

        /// Where inside its pixel the ray is sent, in pixels, `(0, 0)` being the centre.
        ///
        /// **The same axes the pixel index uses**: x to the right and y *down* the image. That is
        /// not the world's up, and writing it the other way round is the mistake to make here — the
        /// reference implementation shipped this with the sign wrong on both axes and the frame
        /// looked entirely plausible, because a wrong jitter still antialiases. It rides along with
        /// the pixel index in the shader for exactly that reason: added to the same number, it
        /// cannot disagree with it.
        ///
        /// Zero is a ray through the pixel's centre, which is every frame that is not being
        /// upscaled or averaged.
        vec2 mJitter;

        /// The angle one pixel subtends, in radians.
        ///
        /// A primary ray is the axis of a cone this wide, and the cone's width where it lands is
        /// what decides which mip a texture is read from. Without it every fetch is level zero, the
        /// mip chains are carried and never read, and everything in the distance crawls.
        ///
        /// **Nought under a parallel projection, and that is not a missing answer.** A parallel
        /// ray's cone never widens; its footprint is one pixel of the box for the whole of its
        /// length, which is what `pixelExtent` reads off `mRight` instead.
        float mSpreadAngle;

        /// Non-zero for a parallel projection rather than a pinhole one.
        ///
        /// **What a map is, and what a viewpoint straight down needs.** With this set, `mRight` and
        /// `mUp` carry the half-extents of a box in world units rather than of the image plane at
        /// unit distance; a pixel's ray *starts* at `mRight * x - mUp * y` from the eye and every
        /// one of them travels along `mForward`. The eye is then a plane and not a point, which is
        /// why the motion vector has no answer under it.
        uint mOrthographic;

        uint mWidth;
        uint mHeight;
    };

    // Pinned for the reason `scene.h` gives: the side that writes these bytes and the side that
    // reads them are different compilers.
#if defined(RTX_HOST) || defined(__METAL_VERSION__)
    static_assert(sizeof(Camera) == 60, "Camera must be scalar-packed on every side");
#endif

#ifdef RTX_HOST
}
#endif

// What both shading languages read and the host does not, for the reason `RTX_SHADER` gives.
//
// **Every result here is built by assigning fields rather than by calling a constructor**, which is
// the one form both languages have: GLSL has `Ray(offset, direction)` and no brace initialiser,
// Metal has `Ray{offset, direction}` and no constructor. Writing it the shared way is what lets the
// Metal backend generate its rays with this rather than with a third copy of the derivation.
#ifndef RTX_HOST

/// Where a pixel's ray starts relative to the eye, and which way it points.
///
/// **Where it starts is what tells the two projections apart, not where it points.** A pinhole fans
/// every ray out of one point, so a pixel's offset turns the direction and the origin is nothing; a
/// parallel one sends them all the same way from wherever on the box the pixel sits, so the same
/// offset is a position instead.
struct Ray
{
    /// From the eye to where this ray begins. Zero under a pinhole projection.
    vec3 mOffset;

    /// Unit.
    vec3 mDirection;
};

/// The ray through `pixel`, whose `xy` is the integer index and to which the jitter and the half
/// are added here — added to *one* number, so which way is down cannot be disagreed about.
RTX_SHADER Ray rayAt(Camera camera, vec2 pixel)
{
    const vec2 uv = (pixel + 0.5 + camera.mJitter) / vec2(float(camera.mWidth), float(camera.mHeight)) * 2.0 - 1.0;

    Ray ray;
    if (camera.mOrthographic != 0u)
    {
        ray.mOffset = camera.mRight * uv.x - camera.mUp * uv.y;
        ray.mDirection = normalize(camera.mForward);

        return ray;
    }

    ray.mOffset = vec3(0.0);

    // **The sum is written out rather than hoisted into a shared term, and that is not an
    // oversight.** Floating-point addition does not associate: `f + (a - b)` and `(f + a) - b`
    // differ in the last place, and a direction that differs in the last place is a hit a texel
    // over once it has been carried thirty thousand units. The two copies of this derivation had
    // already drifted that way — the trace summed left to right and the wavelet hoisted, so the
    // positions the filter reconstructed were never quite the ones that were shaded. This is the
    // trace's association, because the trace is what everything else is judged against.
    ray.mDirection = normalize(camera.mForward + camera.mRight * uv.x - camera.mUp * uv.y);

    return ray;
}

/// How wide a pixel's cone is where the ray starts, and how much wider it gets per unit travelled.
struct Cone
{
    float mWidth;
    float mSpread;
};

/// What a pixel covers, as a cone.
///
/// **A pinhole's starts at a point and widens; a parallel projection's does neither.** Under one, a
/// ray's cone is one pixel of the box wide for the whole of its length, and `mSpreadAngle` is
/// nought — so a caller that reached for the angle instead would read every texture at level zero
/// and reject every filter tap but its own, which is what a map tile used to do. Said once here
/// because the trace and the wavelet both ask, and a rule two shaders each state is a rule they can
/// each get wrong.
RTX_SHADER Cone coneAt(Camera camera)
{
    Cone cone;
    cone.mWidth = camera.mOrthographic != 0u ? 2.0 * length(camera.mRight) / float(camera.mWidth) : 0.0;
    cone.mSpread = camera.mOrthographic != 0u ? 0.0 : camera.mSpreadAngle;

    return cone;
}

/// How far a ray's cone has spread from its axis where it starts, in radians.
///
/// **Half of `mSpreadAngle`, and the half matters.** Everywhere else that number grows a cone's
/// *width* — `resolved` compares it against a wavelength and `coneLod` against a texel area — so a
/// place that wants an angle from the axis has to take half of it, and a disc whose brightness goes
/// as one over the radius squared would be four times wrong otherwise.
RTX_SHADER float pixelBlur(Camera camera)
{
    return 0.5 * camera.mSpreadAngle;
}

#endif

#endif

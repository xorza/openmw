#ifndef OPENMW_COMPONENTS_SCENEUTIL_OFFSCREENFRAMING_H
#define OPENMW_COMPONENTS_SCENEUTIL_OFFSCREENFRAMING_H

#include <variant>

#include <osg/Vec3f>
#include <osg/Vec4f>

namespace SceneUtil
{
    // How an offscreen picture is framed and lit, in the numbers the game already has. The game fills
    // one of these in for the character preview and for a tile of the local map, and whichever
    // renderer draws the picture reads it: two renderers, one description.

    /// A directional light with no position, which is all an offscreen picture is lit by.
    struct FlatLight
    {
        osg::Vec3f mDirection;
        osg::Vec4f mDiffuse;
        osg::Vec4f mAmbient;
    };

    /// A vertical field of view, in degrees.
    struct Perspective
    {
        float mFieldOfView = 0.f;
    };

    /// A box this many world units across, centred on the view direction.
    struct Orthographic
    {
        float mWidth = 0.f;
        float mHeight = 0.f;
    };

    /// How an offscreen picture is projected, and the near and far it is clipped at.
    ///
    /// **One pair, which the spec a caller fills in and the trace that reads it share**: a
    /// projection holds only the numbers its own kind means, so no field means nothing in the
    /// other case.
    struct Framing
    {
        std::variant<Perspective, Orthographic> mProjection;
        float mNear = 1.f;
        float mFar = 10000.f;
    };
}

#endif

#pragma once

#include <cstdint>

#include <osg/Vec4f>

namespace Rtx
{
    /// A vertex's tangent as the device stores it, `Shaders::TANGENT_*`. A tangent of no length packs
    /// to nought, which is no tangent.
    ///
    /// @param tangent `osgUtil::TangentSpaceGenerator`'s: a direction, and in `w` the handedness the
    ///        bitangent `cross(N, T) * w` is taken with.
    std::uint32_t packTangent(const osg::Vec4f& tangent);

    /// The unit direction and the handedness `packed` holds, or nought where it holds none. Read by
    /// the tests and by nothing else: the host never reads a tangent back, and this is what the
    /// device's `unpackTangent` is held to.
    osg::Vec4f unpackTangent(std::uint32_t packed);
}

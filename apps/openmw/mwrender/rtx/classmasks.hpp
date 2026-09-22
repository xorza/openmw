#pragma once

#include <array>
#include <cstdint>

#include <osg/Node>

#include <components/rtx/mesh.hpp>

#include "../vismask.hpp"

namespace MWRender
{
    /// One class of thing the ray tracer sorts a placement by, and the node mask the game puts on
    /// the root of it.
    struct ClassMask
    {
        Rtx::InstanceClass mClass;
        osg::Node::NodeMask mNodes;
    };

    /// The one mapping from the game's masks to the ray tracer's classes, read both ways:
    /// `rayMaskOf` turns a camera's cull mask into the classes its rays meet, and `WorldMirror`
    /// names the roots the walk sorts by. `Static` is what states none of the others, so the walk
    /// is never told its mask.
    inline constexpr std::array<ClassMask, 4> sClassMasks{
        ClassMask{ Rtx::InstanceClass::Static, Mask_Object | Mask_Static | Mask_Terrain | Mask_Groundcover },
        ClassMask{ Rtx::InstanceClass::Actor, Mask_Actor | Mask_Player },
        ClassMask{ Rtx::InstanceClass::Effect, Mask_Effect },
        ClassMask{ Rtx::InstanceClass::FirstPerson, Mask_FirstPerson },
    };

    /// A camera's cull mask as the trace reads it: which `Rtx::InstanceClass`es its rays meet, and
    /// whether it draws the sprites. The one translation, so both renderers read one mask.
    std::uint32_t rayMaskOf(osg::Node::NodeMask cullMask);
}

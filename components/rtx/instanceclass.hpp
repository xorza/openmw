#pragma once

#include <cstdint>

#include "shaders/scene.h"

namespace Rtx
{
    /// What a camera's cull mask sorts a placement by: the rasterizer's `Mask_Actor | Mask_Player`,
    /// `Mask_Effect` and `Mask_FirstPerson`, and `Static` for everything that states none of them —
    /// the objects, the statics, the ground.
    ///
    /// **The innermost stated class stands for the path.** The rasterizer culls on every stated
    /// mask along it, so the two differ for a class stated under another that a camera includes
    /// while leaving the outer one out — an effect under an actor, drawn by a camera with
    /// `Mask_Effect` and without `Mask_Actor`. No camera in the engine has that mask.
    enum class InstanceClass : std::uint8_t
    {
        Static,
        Actor,
        Effect,
        FirstPerson,
    };

    /// The instance-mask bit a class is placed with. Water stands in for the class where the
    /// material is water, in `recordOf`.
    inline std::uint32_t classBit(const InstanceClass what)
    {
        switch (what)
        {
            case InstanceClass::Actor:
                return Shaders::MASK_ACTOR;
            case InstanceClass::Effect:
                return Shaders::MASK_EFFECT;
            case InstanceClass::FirstPerson:
                return Shaders::MASK_FIRST_PERSON;
            case InstanceClass::Static:
                break;
        }

        return Shaders::MASK_STATIC;
    }
}

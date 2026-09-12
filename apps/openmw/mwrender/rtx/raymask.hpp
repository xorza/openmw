#pragma once

#include <cstdint>

#include <osg/Node>

namespace MWRender
{
    /// A camera's cull mask as the trace reads it: which `Rtx::InstanceClass`es its rays meet, and
    /// whether it draws the sprites. `Rtx::Shaders::MASK_*` in `scene.h` names the bits.
    ///
    /// **The one translation, so both renderers read one mask.** The rasterizer culls on
    /// `SceneUtil::Mask_*`; the frame's eye and every picture inside the interface hand their cull
    /// mask here, and the tracer draws what it names.
    std::uint32_t rayMaskOf(osg::Node::NodeMask cullMask);
}

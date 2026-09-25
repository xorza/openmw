#pragma once

#include "shaders/scene.h"

namespace Rtx
{
    /// One light, placed in the world: a lamp, or the fill a magic effect glows with. The device's
    /// own row, because everything in it is derived and the scene uploads its lights as they lie —
    /// `Shaders::GpuLight` says what each field is, and `lightbuilder.hpp` is where one is made.
    using Light = Shaders::GpuLight;
}

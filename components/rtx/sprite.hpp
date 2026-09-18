#pragma once

#include "runs.hpp"
#include "shaders/scene.h"

namespace Rtx
{
    /// One live particle, and one particle system: the device's own rows, because the scene
    /// uploads its sprites and its emitters as they lie. `Shaders::GpuSprite` and
    /// `Shaders::GpuEmitter` say what each field is; `SceneDesc::addEmitter` is where both are made.
    using Sprite = Shaders::GpuSprite;
    using SpriteEmitter = Shaders::GpuEmitter;

    /// Where an emitter's sprites sit in `SceneDesc::sprites`, as the run the emitter was given.
    /// Here and not on the row, because the row is a shared header's and `Run` is the host's.
    inline Run spritesOf(const SpriteEmitter& emitter)
    {
        return Run{ .mOffset = emitter.mFirst, .mCount = emitter.mCount };
    }
}

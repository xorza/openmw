#pragma once

#include <utility>

namespace Rtx
{
    /// Puts `object` back to its default while keeping the room its buffers grew.
    ///
    /// **Every field not named here is reset, and only a buffer needs naming.** A `reuse()` written
    /// as a list of fields resets a new field only if its author remembers; this one resets a new
    /// scalar for free, and a buffer forgotten is a buffer that reallocates, which the allocation
    /// test sees.
    template <class T, class... Buffers>
    void reuseKeeping(T& object, Buffers T::*... buffers)
    {
        const auto empty = [](auto& buffer) {
            if constexpr (requires { buffer.reuse(); })
                buffer.reuse();
            else
                buffer.clear();
        };

        T fresh;
        (std::swap(fresh.*buffers, object.*buffers), ...);
        (empty(fresh.*buffers), ...);
        object = std::move(fresh);
    }
}

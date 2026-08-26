#pragma once

#include <stdexcept>

namespace Rtx
{
    /// Anything that stops the ray tracing renderer starting or running.
    ///
    /// A backend's own failures belong to bring-up: a missing extension, an unsupported format, a
    /// device out of memory. Once a frame is recording, a failure means this code broke a contract,
    /// and it is still thrown rather than asserted so the caller can shut the renderer down and
    /// leave the game running.
    ///
    /// Here rather than with either backend because both throw it and the harness catches one type.
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };
}

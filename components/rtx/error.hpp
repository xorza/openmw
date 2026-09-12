#pragma once

#include <stdexcept>

namespace Rtx
{
    /// Anything that stops the ray tracing renderer starting or running. Once a frame is
    /// recording, a failure means this code broke a contract, and it is still thrown rather than
    /// asserted so the caller can shut the renderer down and leave the game running.
    class Error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /// The half of `Error` that means this machine cannot run the backend, rather than that this
    /// code broke a contract — so a test suite can skip on a machine with no driver and fail on a
    /// fault, where one type for both is a whole GPU suite reporting success after it ran nothing.
    /// Thrown only where the code asked what this machine can do and was told no.
    class Unsupported : public Error
    {
    public:
        using Error::Error;
    };
}

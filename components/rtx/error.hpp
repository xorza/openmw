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

    /// The half of `Error` that means **this machine cannot run the backend**, rather than that this
    /// code broke a contract.
    ///
    /// **The two questions were one type, and a test suite could not tell them apart.** A machine
    /// with no driver, no qualifying device or no DLSS legitimately skips; a format with no recorded
    /// texel size, a shader the build did not write and a scene that outgrew its own table are
    /// faults. `createRenderer` reported every one of them as a reason to skip, so a whole GPU
    /// suite could report success after it ran nothing.
    ///
    /// **Thrown only where the code asked what this machine can do and was told no.** Everything
    /// after bring-up is a contract, and a contract stays `Error`, which leaves a factory as a
    /// throw and a test as a failure.
    class Unsupported : public Error
    {
    public:
        using Error::Error;
    };
}

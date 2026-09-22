#pragma once

#include <stdexcept>

namespace Rtx
{
    // **Three failures and no common base**, so a catch of one never takes another. A contract this
    // code broke is none of them: it is an assert, or `Rtx::contract` where release must not go on.

    /// What the configuration or the installation supplied, and this renderer cannot run with: a
    /// command line, a setting, a shader file, a file the harness reads. All or nothing, so it ends
    /// the run where it is met, and the one menu that can take a mistyped setting back catches it.
    /// Content is not this: a mesh a mod shipped past what one block holds is refused item by item
    /// and the frame goes on, so a reader of content answers with a `Result` and the code that
    /// decides what becomes of it reports the refusal (`Refusals`).
    class InputError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /// This machine cannot run the backend — so a test suite can skip on a machine with no driver
    /// and fail on a fault, where one type for both is a whole GPU suite reporting success after it
    /// ran nothing. Thrown only where the code asked what this machine can do and was told no.
    class Unsupported : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    /// The device or its driver failed while the renderer ran: a lost device, a wait that never
    /// ended, a call that refused. Nothing below the seam catches it, because nothing below the
    /// seam can go on without the device.
    class DeviceError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };
}

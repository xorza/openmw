#pragma once

#include <stdexcept>

namespace Rtx
{
    // **Three failures and no common base**, so a catch of one never takes another. A contract this
    // code broke is none of them: it is an assert, or `Rtx::contract` where release must not go on.

    /// What the world outside this code supplied, and this renderer cannot use: a content file, a
    /// command line, a setting, a file system. Caught where the input enters, and there the input
    /// is skipped, counted and logged once — a mesh a mod shipped past what one block holds is
    /// the mod's business, and the frame goes on without it.
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

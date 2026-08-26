#pragma once

namespace Rtx
{
    struct ValidationOptions;
}

namespace RtxTool
{
    /// One boolean switch off a command line, and whether anyone actually set it.
    ///
    /// The difference matters: a default is what nobody asked for, and something that was only on
    /// because of a default can be turned off by a flag that contradicts it.
    struct CommandSwitch
    {
        bool mValue = false;
        bool mGiven = false;

        /// True only where it was asked for outright.
        bool isAsked() const { return mValue && mGiven; }

        /// True only where it was turned down outright.
        bool isRefused() const { return !mValue && mGiven; }
    };

    /// Which layers a run wants, from the three switches that can ask for them.
    ///
    /// **An explicit `--validation=false` turns off what was only on by default.** The two finer
    /// switches each imply the layers, and both default on outside a Release build — so refusing
    /// the layers while leaving those defaults standing turned nothing off at all, and anyone who
    /// followed the tool's own advice about timing a frame measured one under instrumentation.
    ///
    /// A switch asked for outright still wins: `--validation=false --sync-validation` is a
    /// contradiction, and the more specific half of it is the half that meant something.
    ///
    /// @param windowed a window under GPU-assisted validation loses the device, so a window does
    ///        not take that one by default. Asking for it outright still turns it on.
    Rtx::ValidationOptions chooseValidation(CommandSwitch layers, CommandSwitch sync, CommandSwitch gpu, bool windowed);
}

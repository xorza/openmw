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
    /// switches each imply the layers, and synchronization validation defaults on outside a Release
    /// build — so refusing the layers while leaving that default standing turned nothing off at
    /// all, and anyone who followed the tool's own advice about timing a frame measured one under
    /// instrumentation.
    ///
    /// A switch asked for outright still wins: `--validation=false --sync-validation` is a
    /// contradiction, and the more specific half of it is the half that meant something.
    ///
    /// **GPU-assisted validation is never a default**, which is the one asymmetry here and is the
    /// layer's own instruction: it asks at `vkCreateInstance` not to be run beside the core checks.
    /// Left on by the build it did both — a window lost the device at `vkWaitForFences` on three
    /// runs of four, and a headless run aborted inside the layer's own thread. So it is asked for
    /// by name or it is off, and the caller has nothing to decide.
    Rtx::ValidationOptions chooseValidation(CommandSwitch layers, CommandSwitch sync, CommandSwitch gpu);
}

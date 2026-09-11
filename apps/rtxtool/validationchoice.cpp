#include "validationchoice.hpp"

#include <components/rtx/renderer.hpp>

namespace RtxTool
{
    Rtx::ValidationOptions chooseValidation(CommandSwitch layers, CommandSwitch sync, CommandSwitch gpu)
    {
        Rtx::ValidationOptions options;

        // A refusal of the layers as a whole leaves only what was asked for by name standing.
        options.mSynchronization = layers.isRefused() ? sync.isAsked() : sync.mValue;

        // **Named or off, and a default cannot turn it on either.** The layer asks not to be run
        // beside the core checks and `validationchoice.hpp` says what running both cost, so reading
        // this one by name is what keeps a build default from ever pairing them again.
        options.mGpuAssisted = gpu.isAsked();

        // Either of the two finer switches is a kind of validation, so either implies the layer that
        // carries it.
        options.mEnabled = layers.mValue || options.mSynchronization || options.mGpuAssisted;

        // **Named on the command line, and not merely left on by the build.** A run that asked is a
        // run whose answer is worthless without the layers, so it fails rather than reports nothing.
        options.mDemanded = layers.isAsked() || sync.isAsked() || gpu.isAsked();

        return options;
    }
}

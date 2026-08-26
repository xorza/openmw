#include "validationchoice.hpp"

#include <components/rtx/renderer.hpp>

namespace RtxTool
{
    Rtx::ValidationOptions chooseValidation(CommandSwitch layers, CommandSwitch sync, CommandSwitch gpu, bool windowed)
    {
        // A refusal of the layers as a whole leaves only what was asked for by name standing.
        const auto wanted
            = [refused = layers.isRefused()](CommandSwitch flag) { return refused ? flag.isAsked() : flag.mValue; };

        Rtx::ValidationOptions options;
        options.mSynchronization = wanted(sync);

        // **A window under GPU-assisted validation loses the device**: `vkWaitForFences` comes back
        // `VK_ERROR_DEVICE_LOST`, on three runs of four, somewhere between twenty seconds and a
        // minute in. It is not the shader — three thousand headless traces of the same frame under
        // the same layer are clean — so it is that layer over a swapchain, and the answer for now is
        // not to pay for it where it cannot be had.
        options.mGpuAssisted = windowed ? gpu.isAsked() : wanted(gpu);

        // Either of the two finer switches is a kind of validation, so either implies the layer that
        // carries it.
        options.mEnabled = layers.mValue || options.mSynchronization || options.mGpuAssisted;

        return options;
    }
}

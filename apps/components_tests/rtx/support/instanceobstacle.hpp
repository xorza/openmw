#pragma once

#include <string>

namespace Rtx::Testing
{
    /// Why this machine cannot build a Vulkan instance, or empty where it can.
    ///
    /// The two ways a machine legitimately has nothing to trace with: no loader, or a loader with no
    /// driver behind it — which fails at `vkCreateInstance` with `VK_ERROR_INCOMPATIBLE_DRIVER`
    /// rather than by handing back an empty device list.
    ///
    /// Shared by the harness, which fails its binary on one, and the tests that build an instance of
    /// their own, which skip on one, so the two cannot come to disagree about which failure is
    /// honest and which is a finding. Whether a device that *does* exist qualifies is a different
    /// question, and `PhysicalDevice::select` throws it.
    std::string findInstanceObstacle();
}

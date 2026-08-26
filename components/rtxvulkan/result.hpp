#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// Name of a `VkResult` as it is spelled in the header, for messages.
    std::string_view resultName(VkResult result);

    /// Throws `Error` naming `call` and the result unless `result` is `VK_SUCCESS`.
    ///
    /// `VK_INCOMPLETE` is a failure here. Enumeration loops that can legitimately see it handle it
    /// before calling this.
    void checkVk(VkResult result, const char* call);

    /// How long a wait on the device may take before it is called a failure.
    ///
    /// **Generous, because this is a canary and not a budget.** No honest submit in this renderer
    /// takes a second — the longest measured is a scene rebuild at a fifth of one — so anything that
    /// reaches this is a device that has stopped answering rather than one that is busy.
    inline constexpr std::uint64_t sPatience = 10'000'000'000ull;

    /// Waits for `fences` and throws `Error` naming `what` if the device does not answer in time.
    ///
    /// **A deadline, because the alternative cannot be told from success.** `UINT64_MAX` makes a
    /// device that will never signal and a device still working the same call, forever: a stalled
    /// submit took the whole test suite with it and left nothing but a wedged process and a GPU at
    /// full tilt. A wait that ends says which submit it was, fails one thing, and lets the rest run.
    ///
    /// @param patience nanoseconds to allow. Defaulted so no caller has to think about it, and a
    ///        parameter so the failure can be reached in a test without waiting out the real one.
    void awaitVk(VkDevice device, VkFence fence, const char* what, std::uint64_t patience = sPatience);

    /// What a wait that ran out is called, so the two places that can say it say it the same way.
    std::string timedOut(const char* what, std::uint64_t patience);
}

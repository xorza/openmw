#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    class Device;

    /// Name of a `VkResult` as it is spelled in the header, for messages.
    std::string_view resultName(VkResult result);

    /// Throws `Error` naming `call` and the result unless `result` is `VK_SUCCESS`.
    ///
    /// `VK_INCOMPLETE` is a failure here. Enumeration loops that can legitimately see it handle it
    /// before calling this.
    void checkVk(VkResult result, const char* call);

    /// The same, for a call that can lose the device: a submit, a wait, an acquire or a present.
    /// `VK_ERROR_DEVICE_LOST` carries what the device says about the fault, from
    /// `Device::describeFault`, which is the one moment that question may be asked.
    void checkVk(const Device& device, VkResult result, const char* call);

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
    void awaitVk(const Device& device, VkFence fence, const char* what, std::uint64_t patience = sPatience);

    /// What a wait that ran out is called, so the two places that can say it say it the same way.
    std::string timedOut(const char* what, std::uint64_t patience);

    /// Logs `failure` and what was raised. `tearDown` calls this and nothing else should.
    void reportTornDown(std::string_view failure, const char* raised);

    /// Runs `work` and reports whatever it raises rather than letting it out.
    ///
    /// **What every teardown in this backend goes through, because teardown cannot fail.** A
    /// destructor is `noexcept`, so an exception leaving one is `std::terminate`; a `catch (...)`
    /// tidying up after a failed constructor is the same, since a throw there replaces the failure
    /// it was tidying up after. And what fails in a teardown here is nearly always one thing — a
    /// device that has been lost — which is the moment the report matters most and the moment the
    /// process must not abort over it. `~VulkanRenderer` waited on a lost device and took the
    /// process down on top of the fault description it had just built.
    ///
    /// **One call rather than a rule written in comments.** Four teardowns answered it four ways:
    /// two with a `try` of their own, one with nothing, and `Presenter::destroy` by reaching past
    /// `Device::waitIdle` to the raw entry point — which dodges the throw and the fault report with
    /// it. The one that answered with nothing is the one that aborted.
    ///
    /// **Why the raise is not simply removed instead.** Two kinds of failure reach a teardown here
    /// and only one of them has a return code. A lost device answers with a `VkResult`; an
    /// allocation answers with nothing — `PipelineCache::write` asks for the whole blob, which is
    /// hundreds of megabytes — so no teardown in this backend can be made unable to fail. Nor is
    /// the `VkResult` half free to hand back: `checkVk` is what turns one into a message and
    /// appends the device's own fault description, so a sibling that returned instead would either
    /// build that string to give it back — an allocation, on the path where allocation is the other
    /// failure — or leave each teardown to assemble it, which is how `Presenter::destroy` came to
    /// throw the report away. And a result a caller is free to ignore is four answers again.
    ///
    /// **Named so that misusing it reads wrong.** Wherever a caller can act on a failure, this is
    /// the wrong call and `checkVk` is the right one.
    ///
    /// @param failure the whole clause the log states, which the raised message is appended to.
    template <class Work>
    void tearDown(std::string_view failure, Work&& work)
    {
        try
        {
            std::forward<Work>(work)();
        }
        catch (const std::exception& raised)
        {
            reportTornDown(failure, raised.what());
        }
        catch (...)
        {
            // **The promise is that nothing leaves, so it may not depend on what was thrown.**
            // Nothing in this tree raises anything else, and a promise with a hole in it is not one.
            reportTornDown(failure, "something that is not an exception");
        }
    }
}

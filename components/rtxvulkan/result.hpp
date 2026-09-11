#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/error.hpp>

namespace Rtx
{
    class Device;

    /// Name of a `VkResult` as it is spelled in the header, for messages.
    std::string_view resultName(VkResult result);

    /// Throws `Error` naming `call` and the result unless `result` is `VK_SUCCESS`.
    ///
    /// `VK_INCOMPLETE` is a failure here, and `enumerateVk` is the one caller that answers it
    /// instead of raising it.
    void checkVk(VkResult result, const char* call);

    /// The same, for a call that can lose the device: a submit, a wait, an acquire or a present.
    /// `VK_ERROR_DEVICE_LOST` carries what the device says about the fault, from
    /// `Device::describeFault`, which is the one moment that question may be asked.
    void checkVk(const Device& device, VkResult result, const char* call);

    /// The same as the first, for a bring-up call whose failure means this machine cannot run the
    /// backend: making the instance, or opening the device.
    ///
    /// **Throws `Unsupported`, which is the whole difference.** A loader with no driver behind it
    /// answers `vkCreateInstance` with a failure, and that is a machine to skip rather than a fault
    /// to report — `Rtx::Unsupported` says which of the two a caller is looking at.
    void checkVkSupport(VkResult result, const char* call);

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

    /// What an enumeration whose list never stopped growing is called.
    std::string neverSettled(const char* call);

    /// How many times an enumeration may be told the driver's list is longer than it just said.
    ///
    /// **A bound, for the reason `sPatience` is one.** Asking again with the longer count is the
    /// whole answer to `VK_INCOMPLETE`, and a driver that lengthened its list on every ask would
    /// spin here forever — which cannot be told from work. Four, because a list that has not
    /// settled after three re-reads is not settling.
    inline constexpr int sEnumerationTries = 4;

    /// The two-call enumeration every Vulkan list query is made of, asked until the driver has
    /// nothing left to add.
    ///
    /// **`VK_INCOMPLETE` is a legal answer to the second call, and it is the whole reason this
    /// exists.** The count comes from one call and the elements from the next, so a list that grew
    /// in between leaves the second call unable to say everything and saying so. The backend wrote
    /// that pair out eight times by hand and answered it in none of them, because the rule lived
    /// only in the sentence above `checkVk`: a surface that offered one more format than it had a
    /// moment earlier ended the process during start-up.
    ///
    /// **One call site, so the ninth copy cannot be written without the answer in it.**
    ///
    /// @param call the entry point's own name, for the message a failure carries.
    /// @param enumerate invoked as `(std::uint32_t* count, T* into)`, exactly as the entry point
    ///        takes them.
    /// @param prototype what every element is set to before the fill, which is how a structure that
    ///        has to carry its own `sType` is given one.
    template <class T, class Enumerate>
    std::vector<T> enumerateVk(const char* call, Enumerate&& enumerate, const T& prototype = T{})
    {
        std::vector<T> into;

        for (int asked = 0; asked < sEnumerationTries; ++asked)
        {
            std::uint32_t count = 0;
            checkVk(enumerate(&count, nullptr), call);
            if (count == 0)
                return into;

            into.assign(count, prototype);

            const VkResult filled = enumerate(&count, into.data());
            if (filled == VK_INCOMPLETE)
                continue;

            checkVk(filled, call);

            // **Down to what the fill wrote and never up.** A list that shrank between the two
            // calls leaves elements nothing touched, and a driver that reported more than it was
            // given room for would otherwise be handed back elements that are not there.
            into.resize(std::min(static_cast<std::size_t>(count), into.size()));
            return into;
        }

        throw Error(neverSettled(call));
    }

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

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

    /// Throws `Error` naming `call` and the result unless `result` is `VK_SUCCESS`. `VK_INCOMPLETE`
    /// is a failure here, and `enumerateVk` is the one caller that answers it instead of raising it.
    void checkVk(VkResult result, const char* call);

    /// The same, for a call that can lose the device: a submit, a wait, an acquire or a present.
    /// `VK_ERROR_DEVICE_LOST` carries what the device says about the fault, from
    /// `Device::describeFault`, which is the one moment that question may be asked.
    void checkVk(const Device& device, VkResult result, const char* call);

    /// The same as the first, for a bring-up call whose failure means this machine cannot run the
    /// backend: throws `Unsupported`, which is a machine to skip rather than a fault to report.
    void checkVkSupport(VkResult result, const char* call);

    /// How long a wait on the device may take before it is called a failure — a canary and not a
    /// budget: the longest honest submit measured is a scene rebuild at a fifth of a second.
    inline constexpr std::uint64_t sPatience = 10'000'000'000ull;

    /// Waits for `fences` and throws `Error` naming `what` if the device does not answer in time.
    /// A deadline, because with `UINT64_MAX` a stalled submit took the whole test suite with it.
    ///
    /// @param patience nanoseconds to allow; a parameter so a test can reach the failure.
    void awaitVk(const Device& device, VkFence fence, const char* what, std::uint64_t patience = sPatience);

    /// What a wait that ran out is called, so the two places that can say it say it the same way.
    std::string timedOut(const char* what, std::uint64_t patience);

    /// What an enumeration whose list never stopped growing is called.
    std::string neverSettled(const char* call);

    /// How many times an enumeration may be told the driver's list is longer than it just said,
    /// bounded for the reason `sPatience` is.
    inline constexpr int sEnumerationTries = 4;

    /// The two-call enumeration every Vulkan list query is made of, asked until the driver has
    /// nothing left to add: `VK_INCOMPLETE` is a legal answer to the second call, and a surface
    /// that offered one more format than it had a moment earlier ended the process during
    /// start-up.
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

            // Down to what the fill wrote and never up. A list that shrank between the two
            // calls leaves elements nothing touched, and a driver that reported more than it was
            // given room for would otherwise be handed back elements that are not there.
            into.resize(std::min(static_cast<std::size_t>(count), into.size()));
            return into;
        }

        throw Error(neverSettled(call));
    }

    /// Logs `failure` and what was raised. `tearDown` calls this and nothing else should.
    void reportTornDown(std::string_view failure, const char* raised);

    /// Runs `work` and reports whatever it raises rather than letting it out — what every teardown
    /// in this backend goes through, because a destructor is `noexcept` and what fails in one is
    /// nearly always a lost device, the moment the report matters most: `~VulkanRenderer` waited on
    /// a lost device and took the process down on top of the fault description it had just built.
    /// The raise is not removed instead because an allocation can fail with no return code, and
    /// `checkVk` is what appends the device's fault description. Wherever a caller can act on a
    /// failure, `checkVk` is the right call.
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
            // The promise is that nothing leaves, so it may not depend on what was thrown.
            // Nothing in this tree raises anything else, and a promise with a hole in it is not one.
            reportTornDown(failure, "something that is not an exception");
        }
    }
}

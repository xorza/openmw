#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    /// One message the validation layers reported.
    struct ValidationMessage
    {
        std::string mText;

        /// The thread the offending Vulkan call was made on.
        ///
        /// Validation callbacks fire synchronously on the calling thread, and the test binary runs
        /// tests in parallel against one shared log. Without this, the first test to provoke an
        /// error would fail every test that checked after it.
        std::thread::id mThread;
    };

    /// What happens when the layers report an error.
    enum class ValidationPolicy
    {
        /// Record and log. Only for tests, which provoke errors deliberately and assert on them.
        Log,

        /// Record, log, then `std::abort()`. Everything else uses this: validation is a developer
        /// feature, so anyone who asked for the layers wants the stack where the mistake was made,
        /// not a frame that limps on with undefined contents.
        Abort,
    };

    /// Thread-safe sink for validation errors.
    ///
    /// Printing to stderr is not enough for tests: a render that emits validation errors and still
    /// produces plausible pixels would otherwise pass.
    ///
    /// **Errors only.** Warnings are logged but not stored — nothing reads them, and a long session
    /// would otherwise accumulate them without bound.
    class ValidationLog
    {
    public:
        explicit ValidationLog(ValidationPolicy policy)
            : mPolicy(policy)
        {
        }

        ValidationLog(const ValidationLog&) = delete;
        ValidationLog& operator=(const ValidationLog&) = delete;

        ValidationPolicy getPolicy() const { return mPolicy; }

        /// Called from the Vulkan debug callback, on whichever thread it fires.
        void recordError(std::string&& text);

        /// Errors raised by Vulkan calls made on the calling thread.
        std::vector<ValidationMessage> getErrorsOnThisThread() const;

        void clear();

    private:
        const ValidationPolicy mPolicy;
        mutable std::mutex mMutex;
        std::vector<ValidationMessage> mErrors;
    };

    /// Fills in a messenger description that routes every severity to `log`.
    ///
    /// Returned by value so it can be chained into `VkInstanceCreateInfo::pNext`, which is what
    /// catches errors raised by `vkCreateInstance` and `vkDestroyInstance` themselves.
    VkDebugUtilsMessengerCreateInfoEXT makeMessengerCreateInfo(ValidationLog& log);
}

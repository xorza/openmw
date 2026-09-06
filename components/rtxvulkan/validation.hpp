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

        /// The thread this message is filed under: the one that made the call, or the one
        /// `AdoptedThread` names in its place.
        ///
        /// The test binary runs tests in parallel against one shared log, so without this the first
        /// test to provoke an error would fail every test that collected after it.
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

        /// Appends the errors raised by Vulkan calls made on the calling thread, and removes them.
        ///
        /// **Taken rather than read, and under the one lock.** A message arriving between a read and
        /// a separate clear is a message nobody ever sees, and a collector that reads without
        /// removing hands the same error to the next caller as well.
        ///
        /// The text only: which thread a message was filed under is how this log finds it, and no
        /// caller has anything to do with the answer.
        void takeErrorsOnThisThread(std::vector<std::string>& out);

        void clear();

    private:
        const ValidationPolicy mPolicy;
        mutable std::mutex mMutex;
        std::vector<ValidationMessage> mErrors;
    };

    /// Files this thread's validation errors under `owner` for as long as it stands.
    ///
    /// **What keeps a worker's mistake with the caller that started it.** Pipeline compilation runs
    /// a thread per core and the layers report on whichever thread made the call, so an error raised
    /// inside a worker is filed under a thread nobody ever collects from. The log is filed by thread
    /// at all because the test binary runs tests in parallel against one shared log, so the answer
    /// is to move the message rather than to stop filing.
    class AdoptedThread
    {
    public:
        explicit AdoptedThread(std::thread::id owner);
        ~AdoptedThread();

        AdoptedThread(const AdoptedThread&) = delete;
        AdoptedThread& operator=(const AdoptedThread&) = delete;

    private:
        std::thread::id mPrevious;
    };

    /// Fills in a messenger description that routes every severity to `log`.
    ///
    /// Returned by value so it can be chained into `VkInstanceCreateInfo::pNext`, which is what
    /// catches errors raised by `vkCreateInstance` and `vkDestroyInstance` themselves.
    VkDebugUtilsMessengerCreateInfoEXT makeMessengerCreateInfo(ValidationLog& log);
}

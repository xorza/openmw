#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace Rtx
{
    struct ValidationMessage
    {
        std::string mText;

        /// The thread this message is filed under: the one that made the call, or the one
        /// `AdoptedThread` names in its place. The test binary runs tests in parallel against one
        /// shared log, so the first error would otherwise fail every test that collected after it.
        std::thread::id mThread;
    };

    /// Thread-safe sink for validation errors, because a render that emits them and still draws
    /// plausible pixels would otherwise pass a test. Warnings are logged and not stored: nothing
    /// reads them, and a long session would accumulate them without bound.
    class ValidationLog
    {
    public:
        /// @param abortOnError record, log, then `std::abort()` — what everything but a test asks:
        ///        validation is a developer feature, so anyone who asked for the layers wants the
        ///        stack where the mistake was made, not a frame that limps on with undefined
        ///        contents. A test provokes errors deliberately and asserts on them.
        explicit ValidationLog(const bool abortOnError)
            : mAbortOnError(abortOnError)
        {
        }

        ValidationLog(const ValidationLog&) = delete;
        ValidationLog& operator=(const ValidationLog&) = delete;

        bool abortsOnError() const { return mAbortOnError; }

        /// Called from the Vulkan debug callback, on whichever thread it fires.
        void recordError(std::string&& text);

        /// Appends the errors raised by Vulkan calls made on the calling thread, and removes them
        /// under the one lock, because a message arriving between a read and a separate clear is a
        /// message nobody ever sees.
        void takeErrorsOnThisThread(std::vector<std::string>& out);

        void clear();

    private:
        const bool mAbortOnError;
        mutable std::mutex mMutex;
        std::vector<ValidationMessage> mErrors;
    };

    /// Files this thread's validation errors under `owner` for as long as it stands. Pipeline
    /// compilation runs a thread per core and the layers report on whichever thread made the call,
    /// so an error raised inside a worker would be filed under a thread nobody collects from.
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

    /// Fills in a messenger description that routes every severity to `log`. Returned by value so
    /// it can be chained into `VkInstanceCreateInfo::pNext`, which is what catches errors raised
    /// by `vkCreateInstance` and `vkDestroyInstance` themselves.
    VkDebugUtilsMessengerCreateInfoEXT makeMessengerCreateInfo(ValidationLog& log);
}

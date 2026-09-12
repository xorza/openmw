#include "validation.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include <components/debug/debuglog.hpp>

namespace Rtx
{
    namespace
    {
        VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT /*types*/, const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
        {
            ValidationLog& log = *static_cast<ValidationLog*>(userData);
            const char* const id = data->pMessageIdName != nullptr ? data->pMessageIdName : "?";

            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            {
                std::string text(id);
                text += ": ";
                text += data->pMessage;
                Log(Debug::Error) << "Vulkan validation: " << text;
                log.recordError(std::move(text));

                if (log.getPolicy() == ValidationPolicy::Abort)
                {
                    Log(Debug::Error) << "Aborting: validation errors are fatal when the layers are enabled.";
                    std::abort();
                }
            }
            else
                Log(Debug::Warning) << "Vulkan validation: " << id << ": " << data->pMessage;

            // The spec reserves a true return for the layers' own use; applications must return false.
            return VK_FALSE;
        }

        /// Which thread a message raised on this one is filed under: this thread by default, and
        /// `AdoptedThread` is what moves it. A thread-local rather than a member of the log,
        /// because the callback reaches the log through `pUserData` and knows nothing else, and
        /// because one process may hold more than one instance.
        thread_local std::thread::id sFiledUnder = std::this_thread::get_id();
    }

    void ValidationLog::recordError(std::string&& text)
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mErrors.push_back(ValidationMessage{ std::move(text), sFiledUnder });
    }

    void ValidationLog::takeErrorsOnThisThread(std::vector<std::string>& out)
    {
        const std::thread::id current = std::this_thread::get_id();

        const std::lock_guard<std::mutex> lock(mMutex);

        // Stable, so that messages come back in the order the layers raised them: the first is
        // usually the mistake and the rest are what it led to.
        const auto taken = std::stable_partition(mErrors.begin(), mErrors.end(),
            [current](const ValidationMessage& message) { return message.mThread != current; });

        for (auto at = taken; at != mErrors.end(); ++at)
            out.push_back(std::move(at->mText));

        mErrors.erase(taken, mErrors.end());
    }

    void ValidationLog::clear()
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mErrors.clear();
    }

    AdoptedThread::AdoptedThread(std::thread::id owner)
        : mPrevious(sFiledUnder)
    {
        sFiledUnder = owner;
    }

    AdoptedThread::~AdoptedThread()
    {
        sFiledUnder = mPrevious;
    }

    VkDebugUtilsMessengerCreateInfoEXT makeMessengerCreateInfo(ValidationLog& log)
    {
        return VkDebugUtilsMessengerCreateInfoEXT{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            // Errors and warnings only. Info severity is where the loader narrates every manifest it
            // reads, which buries the two severities anyone acts on. VK_LOADER_DEBUG covers that case.
            .messageSeverity
            = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = &onMessage,
            .pUserData = &log,
        };
    }
}

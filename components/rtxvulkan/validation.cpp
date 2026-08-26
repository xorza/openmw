#include "validation.hpp"

#include <cstdlib>

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
    }

    void ValidationLog::recordError(std::string&& text)
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mErrors.push_back(ValidationMessage{ std::move(text), std::this_thread::get_id() });
    }

    std::vector<ValidationMessage> ValidationLog::getErrorsOnThisThread() const
    {
        const std::thread::id current = std::this_thread::get_id();
        std::vector<ValidationMessage> result;

        const std::lock_guard<std::mutex> lock(mMutex);
        for (const ValidationMessage& message : mErrors)
            if (message.mThread == current)
                result.push_back(message);

        return result;
    }

    void ValidationLog::clear()
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mErrors.clear();
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

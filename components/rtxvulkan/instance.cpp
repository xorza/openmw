#include "instance.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include <components/debug/debuglog.hpp>

#include <components/rtx/error.hpp>

#include "dlss.hpp"
#include "requirements.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        constexpr const char* sValidationLayer = "VK_LAYER_KHRONOS_validation";

        bool hasLayer(const char* name)
        {
            std::uint32_t count = 0;
            checkVk(vkEnumerateInstanceLayerProperties(&count, nullptr), "vkEnumerateInstanceLayerProperties");
            std::vector<VkLayerProperties> layers(count);
            checkVk(vkEnumerateInstanceLayerProperties(&count, layers.data()), "vkEnumerateInstanceLayerProperties");

            return std::any_of(layers.begin(), layers.end(),
                [&](const VkLayerProperties& layer) { return std::strcmp(layer.layerName, name) == 0; });
        }

        bool hasInstanceExtension(const char* name)
        {
            std::uint32_t count = 0;
            checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
                "vkEnumerateInstanceExtensionProperties");
            std::vector<VkExtensionProperties> extensions(count);
            checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()),
                "vkEnumerateInstanceExtensionProperties");

            return std::any_of(extensions.begin(), extensions.end(), [&](const VkExtensionProperties& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        }
    }

    InstanceOptions toInstanceOptions(const ValidationOptions& validation)
    {
        return InstanceOptions{
            .mValidation = validation.mEnabled,
            .mSynchronizationValidation = validation.mSynchronization,
            .mGpuAssistedValidation = validation.mGpuAssisted,
            .mPolicy = validation.mAbortOnError ? ValidationPolicy::Abort : ValidationPolicy::Log,
        };
    }

    Instance::Instance(const InstanceOptions& options)
    {
        checkVk(vkEnumerateInstanceVersion(&mApiVersion), "vkEnumerateInstanceVersion");
        if (mApiVersion < sApiVersion)
            throw Error("the Vulkan loader offers " + versionString(mApiVersion) + ", and this renderer is written "
                "against " + versionString(sApiVersion));

        std::vector<const char*> extensions(options.mSurfaceExtensions);
#ifdef OPENMW_RTX_DLSS
        // NGX names instance extensions of its own, and will not start without them.
        for (const char* const name : Dlss::getInstanceExtensions())
            extensions.push_back(name);
#endif
        std::vector<const char*> layers;

        // Object names and command-buffer labels are what make a capture readable, and a capture is
        // most wanted on a run that is not carrying the layers, so the extension is asked for
        // whenever this build names anything.
#ifdef OPENMW_RTX_DEBUG_NAMES
        const bool wantDebugUtils = true;
#else
        const bool wantDebugUtils = options.mValidation;
#endif
        mDebugUtils = wantDebugUtils && hasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        // Validation reaches us only through the messenger, so without the extension it would run
        // and report nothing — worse than not running at all, because the clean output would read
        // as a pass.
        const bool validation = options.mValidation && mDebugUtils && hasLayer(sValidationLayer);
        if (options.mValidation && !validation)
            Log(Debug::Warning) << "Vulkan validation was requested but " << sValidationLayer << " or "
                                << VK_EXT_DEBUG_UTILS_EXTENSION_NAME << " is missing.";

        if (mDebugUtils)
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        if (validation)
        {
            mValidationLog = std::make_unique<ValidationLog>(options.mPolicy);
            layers.push_back(sValidationLayer);
        }

        const VkApplicationInfo application{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "OpenMW",
            .applicationVersion = 0,
            .pEngineName = "OpenMW RTX",
            .engineVersion = 0,
            .apiVersion = sApiVersion,
        };

        // Chained into the create info so errors raised by vkCreateInstance and vkDestroyInstance
        // themselves are reported; the standalone messenger below covers everything in between.
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
        std::vector<VkValidationFeatureEnableEXT> enabled;
        VkValidationFeaturesEXT validationFeatures{};

        // What GPU-assisted validation does about reads past the end of a buffer.
        //
        /// **Its own instrumentation is what makes it unaffordable here**, and it says so: the layer
        // warns that a shader with this many storage buffers "will be very slow to compile and
        // runtime performance may also be slow", and points at this setting. Left alone it is worse
        // than slow — a window under GPU-AV loses the device inside half a minute.
        //
        // Turning it on hands the same job to the hardware's own robust buffer access, which returns
        // zero for a read past the end instead of instrumenting every access to catch it. What is
        // given up is the *report*; what is kept is everything else GPU-AV checks, including what a
        // ray query does with its own arguments — which is what it caught here first.
        const VkBool32 robustness = VK_TRUE;
        const VkLayerSettingEXT robustSetting{
            .pLayerName = sValidationLayer,
            .pSettingName = "gpuav_force_on_robustness",
            .type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
            .valueCount = 1,
            .pValues = &robustness,
        };
        VkLayerSettingsCreateInfoEXT layerSettings{};

        const void* next = nullptr;

        if (validation)
        {
            messengerInfo = makeMessengerCreateInfo(*mValidationLog);
            next = &messengerInfo;

            if (options.mSynchronizationValidation)
                enabled.push_back(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
            if (options.mGpuAssistedValidation)
                enabled.push_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);

            if (!enabled.empty())
            {
                validationFeatures = VkValidationFeaturesEXT{
                    .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
                    .pNext = &messengerInfo,
                    .enabledValidationFeatureCount = static_cast<std::uint32_t>(enabled.size()),
                    .pEnabledValidationFeatures = enabled.data(),
                };
                next = &validationFeatures;
            }

            if (options.mGpuAssistedValidation)
            {
                layerSettings = VkLayerSettingsCreateInfoEXT{
                    .sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
                    .pNext = next,
                    .settingCount = 1,
                    .pSettings = &robustSetting,
                };
                next = &layerSettings;
            }
        }

        const VkInstanceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = next,
            .pApplicationInfo = &application,
            .enabledLayerCount = static_cast<std::uint32_t>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };

        checkVk(vkCreateInstance(&createInfo, nullptr, &mHandle), "vkCreateInstance");

        // A constructor that throws runs no destructor, and failing to start the renderer is a
        // supported outcome rather than the end of the process — the game carries on with OpenGL.
        // So anything after a successful create cleans up before it rethrows.
        try
        {
            if (validation)
            {
                const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(mHandle, "vkCreateDebugUtilsMessengerEXT"));
                if (create == nullptr)
                    throw Error("the validation layer is loaded but vkCreateDebugUtilsMessengerEXT is missing");

                checkVk(create(mHandle, &messengerInfo, nullptr, &mMessenger), "vkCreateDebugUtilsMessengerEXT");
            }
        }
        catch (...)
        {
            vkDestroyInstance(mHandle, nullptr);
            mHandle = VK_NULL_HANDLE;
            throw;
        }
    }

    Instance::~Instance()
    {
        if (mMessenger != VK_NULL_HANDLE)
        {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(mHandle, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy != nullptr)
                destroy(mHandle, mMessenger, nullptr);
        }

        if (mHandle != VK_NULL_HANDLE)
            vkDestroyInstance(mHandle, nullptr);
    }
}

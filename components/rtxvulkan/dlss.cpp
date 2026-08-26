#include "dlss.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>

#include <components/rtx/error.hpp>

#include "device.hpp"
#include "ngx.hpp"
#include "physicaldevice.hpp"

namespace Rtx
{
    namespace
    {
        /// Somewhere NGX may write its own files.
        ///
        /// **Not the same thing as where its feature libraries live**, which is the mistake to make:
        /// this is a writable directory of NGX's own, and the libraries are found through the search
        /// list in `NVSDK_NGX_FeatureCommonInfo`. Handing it the library directory instead fails at
        /// `Init` with `FAIL_InvalidParameter`, which names no parameter.
        const wchar_t* dataPath()
        {
            static const std::wstring path = [] {
                const std::filesystem::path where = std::filesystem::temp_directory_path() / "openmw-rtx-ngx";
                std::error_code ignored;
                std::filesystem::create_directories(where, ignored);

                const std::string given = where.string();
                return std::wstring(given.begin(), given.end());
            }();

            return path.c_str();
        }

        /// Where NGX finds the feature libraries it loads at runtime.
        ///
        /// Its own search is the application's folder alone, and these are nowhere near the binary.
        const wchar_t* featurePath()
        {
            static const std::wstring path = [] {
                const std::string_view given = OPENMW_RTX_NGX_FEATURES;
                return std::wstring(given.begin(), given.end());
            }();

            return path.c_str();
        }

        /// **Off unless asked for, and that is not timidity.** NGX's feature libraries write around
        /// a thousand lines to the console on one successful run — enough to bury the message of
        /// whatever failure sent someone looking for them. The reference implementation found the
        /// one error that mattered only in this log, and it appears nowhere in the API surface.
        NVSDK_NGX_Logging_Level loggingLevel()
        {
            return std::getenv("OPENMW_RTX_NGX_LOG") != nullptr ? NVSDK_NGX_LOGGING_LEVEL_ON
                                                                : NVSDK_NGX_LOGGING_LEVEL_OFF;
        }
    }

    std::span<const char* const> Dlss::getInstanceExtensions()
    {
        unsigned int instanceCount = 0;
        const char** instance = nullptr;
        unsigned int deviceCount = 0;
        const char** device = nullptr;

        if (NVSDK_NGX_VULKAN_RequiredExtensions(&instanceCount, &instance, &deviceCount, &device)
            != NVSDK_NGX_Result_Success)
            throw Error("NGX would not say which instance extensions it needs");

        return std::span<const char* const>(instance, instanceCount);
    }

    std::span<const char* const> Dlss::getDeviceExtensions()
    {
        unsigned int instanceCount = 0;
        const char** instance = nullptr;
        unsigned int deviceCount = 0;
        const char** device = nullptr;

        if (NVSDK_NGX_VULKAN_RequiredExtensions(&instanceCount, &instance, &deviceCount, &device)
            != NVSDK_NGX_Result_Success)
            throw Error("NGX would not say which device extensions it needs");

        return std::span<const char* const>(device, deviceCount);
    }

    DlssSupport Dlss::probe(const Device& device, VkInstance instance)
    {
        if (sLive != nullptr)
        {
            if (sLive->mDevice != device.getHandle())
                return DlssSupport{ false, "NGX is up on another device, and it keeps one runtime per process" };

            return DlssSupport{ sLive->mAvailable, sLive->mObstacle };
        }

        // **Stood up and taken down inside this call**, which is what makes it safe to ask from
        // anywhere: nothing outside holds a runtime that this could be ending, because the branch
        // above is what happens when something does.
        const Dlss asked(device, instance);
        return DlssSupport{ asked.mAvailable, asked.mObstacle };
    }

    Dlss::Dlss(const Device& device, VkInstance instance)
        : mDevice(device.getHandle())
    {
        // **Before anything is started**, so a refusal leaves the runtime that is up untouched. A
        // constructor that threw after `Init` would have shut the first one down on the way out.
        if (sLive != nullptr)
            throw Error("NGX keeps one runtime per process and one is already up");

        const wchar_t* const searched[] = { featurePath() };

        NVSDK_NGX_FeatureCommonInfo common{};
        common.PathListInfo.Path = searched;
        common.PathListInfo.Length = 1;
        common.LoggingInfo.LoggingCallback = nullptr;
        common.LoggingInfo.MinimumLoggingLevel = loggingLevel();
        common.LoggingInfo.DisableOtherLoggingSinks = false;

        // NVIDIA's handle on an application, for their own telemetry and driver overrides.
        //
        // **A GUID, and parsed as one.** The driver checks the shape and nothing else: a readable
        // name here comes back from `Init` as `FAIL_InvalidParameter`, which names no parameter and
        // writes nothing to the log. This one is this fork's own rather than borrowed, and the
        // engine is `CUSTOM` because OpenMW is not one of the engines NVIDIA knows.
        const NVSDK_NGX_Result started = NVSDK_NGX_VULKAN_Init_with_ProjectID("c541dbdf-6e4f-4476-ad27-15d2b4a231f4",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.52", dataPath(), instance, device.getPhysicalDevice().getHandle(), mDevice,
            vkGetInstanceProcAddr, vkGetDeviceProcAddr, &common, NVSDK_NGX_Version_API);

        if (NVSDK_NGX_FAILED(started))
            throw Error("NGX would not start: " + describeNgxResult(started));

        const NVSDK_NGX_Result asked = NVSDK_NGX_VULKAN_GetCapabilityParameters(&mCapabilities);
        if (NVSDK_NGX_FAILED(asked) || mCapabilities == nullptr)
        {
            // A constructor that throws gets no destructor, and NGX is up: leaving it that way
            // would refuse every later attempt for a reason that is no longer true.
            NVSDK_NGX_VULKAN_Shutdown1(mDevice);
            throw Error("NGX started and would not say what it can do: " + describeNgxResult(asked));
        }

        int available = 0;
        mCapabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
        mAvailable = available != 0;

        if (!mAvailable)
        {
            int needsDriver = 0;
            mCapabilities->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &needsDriver);
            mObstacle = needsDriver != 0 ? "this driver is older than the Ray Reconstruction it would have to load"
                                         : "this device does not offer Ray Reconstruction";
        }

        // **Last, so that only a runtime that came all the way up claims the process.** Everything
        // above throws on failure, and a constructor that threw gets no destructor to clear this.
        sLive = this;
    }

    VkExtent2D Dlss::getRenderSize(VkExtent2D output, Upscale upscale) const
    {
        // Dynamic resolution is not something this renderer does — the trace's targets are made
        // once per size — so the range the query also fills in is read and discarded.
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int mostWide = 0;
        unsigned int mostTall = 0;
        unsigned int leastWide = 0;
        unsigned int leastTall = 0;
        float sharpness = 0.0f;

        // **The query is a function pointer inside the capability map, not an exported symbol** —
        // the driver's feature library puts it there. So it is absent exactly when that library was
        // not found, and the helper answers `FAIL_OutOfDate` rather than anything about paths.
        const NVSDK_NGX_Result asked = NGX_DLSSD_GET_OPTIMAL_SETTINGS(mCapabilities, output.width, output.height,
            ngxQualityOf(upscale), &width, &height, &mostWide, &mostTall, &leastWide, &leastTall, &sharpness);

        if (NVSDK_NGX_FAILED(asked))
            throw Error("DLSS would not say what to render at: " + describeNgxResult(asked));

        if (width == 0 || height == 0)
            throw Error("DLSS answered with an empty render size");

        return VkExtent2D{ width, height };
    }

    Dlss::~Dlss()
    {
        sLive = nullptr;

        // The capability map is NGX's own and goes with it; nothing releases it separately.
        NVSDK_NGX_VULKAN_Shutdown1(mDevice);
    }
}

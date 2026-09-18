#include "drivershadercache.hpp"

#include <cstdlib>
#include <string_view>

#include <components/platform/process.hpp>

namespace Rtx
{
    namespace
    {
        /// The word NVIDIA's driver reads its shader disk cache on and off by.
        constexpr const char* sDriverShaderCache = "__GL_SHADER_DISK_CACHE";
    }

    bool refuseDriverShaderCache()
    {
        return Platform::Process::setEnvironmentDefault(sDriverShaderCache, "0");
    }

    bool driverShaderCacheRefused()
    {
        const char* const word = std::getenv(sDriverShaderCache);
        return word != nullptr && std::string_view(word) == "0";
    }
}

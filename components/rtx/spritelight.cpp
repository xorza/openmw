#include "spritelight.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::string_view sPrefix = "sprite/";
    }

    std::string SpriteLightMap::keyFor(VFS::Path::NormalizedView source)
    {
        std::string key(sPrefix);
        key += source.value();
        return key;
    }

    std::optional<VFS::Path::Normalized> SpriteLightMap::sourceOf(std::string_view key)
    {
        if (!key.starts_with(sPrefix))
            return std::nullopt;

        return VFS::Path::Normalized(key.substr(sPrefix.size()));
    }
}

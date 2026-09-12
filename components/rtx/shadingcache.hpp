#pragma once

#include <string>
#include <unordered_map>

#include "shadingmap.hpp"

namespace Rtx
{
    struct TextureData;

    /// The painted light of each texture that was estimated, kept by the file it came from,
    /// because estimating one reads every texel of the largest level and the same handful of
    /// ground textures make every chunk of a region. Not the texture builder's cache, which drops
    /// a description with its slot: a chunk baked from a ground texture no slot names any more is
    /// still a chunk.
    class ShadingCache
    {
    public:
        /// The estimate for `texture`, made once for each `file` and made afresh for every texture
        /// that names none. The reference is good until the next call that names no file.
        const ShadingMap& estimate(const TextureData& texture, const std::string& file)
        {
            if (file.empty())
            {
                mUnnamed = ShadingMap(texture);
                return mUnnamed;
            }

            return mPainted.try_emplace(file, texture).first->second;
        }

    private:
        std::unordered_map<std::string, ShadingMap> mPainted;

        /// The estimate of a texture with no file to key it by, held only until the next one.
        ShadingMap mUnnamed;
    };
}

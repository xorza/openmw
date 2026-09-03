#pragma once

#include <string>
#include <unordered_map>

#include "shadingmap.hpp"

namespace Rtx
{
    struct TextureData;

    /// The painted light of each texture that has been estimated, kept by the file it came from.
    ///
    /// **Estimating one reads every texel of a texture's largest level**, and the same handful of
    /// ground textures make every chunk of a region — so a caller that estimates per chunk would
    /// read the same megabyte once per chunk it bakes. That repeated read is the 5% of a crossing's
    /// CPU `texturebuilder.hpp` names.
    ///
    /// **Not the same cache as the texture builder's, and it should not become one.** That one
    /// keeps a description per live slot and drops it when the slot goes; this one is keyed on a
    /// path and lives as long as its owner, because what it answers outlives any slot — a chunk
    /// baked from a ground texture no slot names any more is still a chunk.
    class ShadingCache
    {
    public:
        /// The estimate for `texture`, made once for each `file` and made afresh for every texture
        /// that names none.
        ///
        /// The reference is good until the next call that names no file.
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

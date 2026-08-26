#ifndef OPENMW_COMPONENTS_ESMLOADER_LOAD_H
#define OPENMW_COMPONENTS_ESMLOADER_LOAD_H

#include "esmdata.hpp"

#include <components/esm3/esmreader.hpp>

#include <string>
#include <vector>

namespace ToUTF8
{
    class Utf8Encoder;
}

namespace Files
{
    class Collections;
}

namespace Loading
{
    class Listener;
}

namespace EsmLoader
{
    inline constexpr std::size_t fileProgress = 1000;

    /// What to read out of the content files. Everything not asked for is skipped as it is met.
    struct Query
    {
        bool mLoadCells = false;
        bool mLoadGameSettings = false;
        bool mLoadLands = false;
        bool mLoadLandTextures = false;
        bool mLoadRegions = false;

        /// Which model-bearing record types to read; see `modelRecords` and `allModelRecords`.
        ModelRecordMask mModels;
    };

    EsmData loadEsmData(const Query& query, const std::vector<std::string>& contentFiles,
        const Files::Collections& fileCollections, ESM::ReadersCache& readers, ToUTF8::Utf8Encoder* encoder,
        Loading::Listener* listener = nullptr);
}

#endif

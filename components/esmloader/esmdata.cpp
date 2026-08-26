#include "esmdata.hpp"

#include "lessbyid.hpp"

#include <components/esm/defs.hpp>
// The three types `esmdata.hpp` only forward-declares. `~EsmData` is here, so it destroys their
// vectors here and needs them complete.
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/variant.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EsmLoader
{
    namespace
    {
        /// Whether `records` is the table `type` names, filling in `model` when it also holds `refId`.
        ///
        /// Answering "wrong table" and "right table, no such record" differently is what lets the
        /// fold below stop at the first table that matches instead of searching all of them.
        template <class T>
        bool findModel(const std::vector<T>& records, const ESM::RefId& refId, ESM::RecNameInts type,
            VFS::Path::NormalizedView& model)
        {
            if (T::sRecordId != type)
                return false;

            const auto it = std::lower_bound(records.begin(), records.end(), refId, LessById{});
            if (it != records.end() && it->mId == refId)
                model = it->mModel.getNormalized();

            return true;
        }
    }

    EsmData::~EsmData() {}

    std::vector<LandTextureRef> prepareLandTextures(std::span<const LandTextureRecord> records)
    {
        std::unordered_map<ESM::RefId, VFS::Path::Normalized> byId;
        for (const LandTextureRecord& record : records)
            if (!record.mDeleted)
                byId[record.mId] = record.mTexture;

        std::vector<LandTextureRef> result;
        result.reserve(records.size());
        for (const LandTextureRecord& record : records)
            if (!record.mDeleted)
                result.push_back(LandTextureRef{
                    .mPlugin = record.mPlugin,
                    .mIndex = record.mIndex,
                    .mTexture = byId[record.mId],
                });

        // Stable, and `unique` keeps the front of each group, so between them the first claim on a
        // pair is the one that survives.
        const auto byKey = [](const LandTextureRef& l, const LandTextureRef& r) { return l.getKey() < r.getKey(); };
        const auto sameKey = [](const LandTextureRef& l, const LandTextureRef& r) { return l.getKey() == r.getKey(); };
        std::stable_sort(result.begin(), result.end(), byKey);
        result.erase(std::unique(result.begin(), result.end(), sameKey), result.end());

        return result;
    }

    const VFS::Path::Normalized* getLandTexture(const EsmData& content, std::uint32_t index, int plugin)
    {
        const std::pair<int, std::uint32_t> key(plugin, index);
        const auto it = std::lower_bound(content.mLandTextures.begin(), content.mLandTextures.end(), key,
            [](const LandTextureRef& l, const std::pair<int, std::uint32_t>& r) { return l.getKey() < r; });

        if (it == content.mLandTextures.end() || it->getKey() != key)
            return nullptr;

        return &it->mTexture;
    }

    VFS::Path::NormalizedView getModel(const EsmData& content, const ESM::RefId& refId, ESM::RecNameInts type)
    {
        // The view points into the record that was found, which outlives this call because it is
        // owned by `content`. Copying the path into a local here would return a dangling view.
        VFS::Path::NormalizedView model;
        std::apply([&](const auto&... records) { (findModel(records, refId, type, model) || ...); }, content.mModels);
        return model;
    }

    ESM::Variant getGameSetting(const std::vector<ESM::GameSetting>& records, std::string_view id)
    {
        auto it = std::lower_bound(records.begin(), records.end(), id, LessById{});
        if (it == records.end() || it->mId != id)
            throw std::runtime_error("Game settings \"" + std::string(id) + "\" is not found");
        return it->mValue;
    }
}

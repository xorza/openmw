#ifndef OPENMW_COMPONENTS_ESMLOADER_ESMDATA_H
#define OPENMW_COMPONENTS_ESMLOADER_ESMDATA_H

#include <components/esm/defs.hpp>
#include <components/esm/refid.hpp>
#include <components/esmloader/lessbyid.hpp>
#include <components/misc/tuplemeta.hpp>
#include <components/vfs/pathutil.hpp>

// The record types of `ModelRecords`, whose definitions the list below needs and whose headers are
// therefore part of it. Forward declarations would do for the tuple itself and not for the folds
// over it, which read `sRecordId`, `mId` and `mModel` off each type.
#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadalch.hpp>
#include <components/esm3/loadappa.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbody.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadingr.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadlock.hpp>
#include <components/esm3/loadmisc.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadprob.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/esm3/loadrepa.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/loadweap.hpp>

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ESM
{
    struct Cell;
    struct GameSetting;
    struct Land;
    class Variant;
}

namespace EsmLoader
{
    struct RefIdWithType
    {
        ESM::RefId mId;
        ESM::RecNameInts mType;
    };

    /// Every record type that names a model, in one list.
    ///
    /// They are read, merged and looked up identically, so this list is the only thing that
    /// distinguishes them: the reader, the store, the query and the model lookup are all generated
    /// from it, and adding a type is this line alone. Cells, lands and game settings are absent
    /// because each keys on something other than a record id and has its own path.
    ///
    /// `ESM::NPC` is here for what it *names* rather than for what it draws: its own `mModel` is
    /// almost always empty, and a person is assembled out of the `ESM::BodyPart` records their race
    /// and sex call for. Both are in the list because that assembly needs to look both up by id, and
    /// looking a record up by id is exactly what this list generates.
    using ModelRecords = std::tuple<ESM::Activator, ESM::Apparatus, ESM::Armor, ESM::BodyPart, ESM::Book, ESM::Clothing,
        ESM::Container, ESM::Creature, ESM::Door, ESM::Ingredient, ESM::Light, ESM::Lockpick, ESM::Miscellaneous,
        ESM::NPC, ESM::Potion, ESM::Probe, ESM::Repair, ESM::Static, ESM::Weapon>;

    /// Which of `ModelRecords` a caller wants, one bit each.
    ///
    /// Selective rather than all-or-nothing because the answer decides what the caller's world is
    /// made of: a navmesh built with the lights and the books in it is a different navmesh.
    using ModelRecordMask = std::bitset<std::tuple_size_v<ModelRecords>>;

    namespace Details
    {
        template <class T>
        struct AsVectors;

        template <class... Ts>
        struct AsVectors<std::tuple<Ts...>>
        {
            using Type = std::tuple<std::vector<Ts>...>;
        };
    }

    /// One vector per type of `ModelRecords`, each sorted by record id.
    using ModelStore = Details::AsVectors<ModelRecords>::Type;

    /// The mask naming exactly `Ts`, which the compiler checks against the list.
    template <class... Ts>
    ModelRecordMask modelRecords()
    {
        ModelRecordMask mask;
        (mask.set(Misc::TupleTypeIndex<Ts, ModelRecords>::value), ...);
        return mask;
    }

    /// Every model-bearing type there is, for a caller that places whatever a cell holds.
    inline ModelRecordMask allModelRecords()
    {
        return ModelRecordMask().set();
    }

    /// A land texture as the terrain asks for one.
    ///
    /// Keyed by the plugin that wrote the record and the index it held there, not by record id,
    /// because that is how a cell refers to one: a blend map indexes into its own plugin's texture
    /// list, so index five names a different texture in every file that has one. The id the two
    /// were connected through is resolved away at load — see `EsmData::mLandTextures`.
    struct LandTextureRef
    {
        int mPlugin = 0;
        std::uint32_t mIndex = 0;
        VFS::Path::Normalized mTexture;

        /// What the terrain looks one up by, and what the vector is sorted on.
        std::pair<int, std::uint32_t> getKey() const { return { mPlugin, mIndex }; }
    };

    /// One land texture record as it was read, with the plugin it came from.
    ///
    /// The plugin is half of the key a cell refers to it by and is nowhere in the record itself, so
    /// it has to travel alongside from the moment the reader knows it.
    struct LandTextureRecord
    {
        ESM::RefId mId;
        std::uint32_t mIndex = 0;
        VFS::Path::Normalized mTexture;
        int mPlugin = 0;
        bool mDeleted = false;
    };

    /// Resolves land texture records into the table `getLandTexture` searches.
    ///
    /// Two merge rules meet here and they point opposite ways. A record **id** can be redefined by a
    /// later plugin and the last definition is the texture, so a replacer that changes what "sand"
    /// looks like changes it everywhere. A **(plugin, index)** pair cannot be redefined at all — the
    /// first record to claim one keeps it — because the pair names a slot in one file's own list and
    /// a later file has a list of its own. Both tables are complete before anything is asked, so
    /// composing them here answers exactly as resolving at each query would.
    std::vector<LandTextureRef> prepareLandTextures(std::span<const LandTextureRecord> records);

    struct EsmData
    {
        std::vector<ESM::Cell> mCells;
        std::vector<ESM::GameSetting> mGameSettings;
        std::vector<ESM::Land> mLands;

        /// Sorted by id. What a cell names to say which weathers ever happen over it.
        std::vector<ESM::Region> mRegions;
        std::vector<RefIdWithType> mRefIdTypes;

        /// Sorted by `LandTextureRef::getKey`.
        std::vector<LandTextureRef> mLandTextures;

        /// Read through `get`; it is public because the loader fills it in by folding over the list.
        ModelStore mModels;

        EsmData() = default;
        EsmData(const EsmData&) = delete;
        EsmData(EsmData&&) = default;

        ~EsmData();

        /// Every record of one model-bearing type, sorted by id.
        template <class T>
        const std::vector<T>& get() const
        {
            return std::get<std::vector<T>>(mModels);
        }

        template <class T>
        std::vector<T>& get()
        {
            return std::get<std::vector<T>>(mModels);
        }
    };

    /// The record of one model-bearing type with this id, or null.
    ///
    /// Every table in `ModelStore` is sorted by id, and this is the one place that knows it.
    template <class T>
    const T* find(const EsmData& content, const ESM::RefId& refId)
    {
        const std::vector<T>& records = content.get<T>();
        const auto it = std::lower_bound(records.begin(), records.end(), refId, LessById{});
        return it != records.end() && it->mId == refId ? &*it : nullptr;
    }

    VFS::Path::NormalizedView getModel(const EsmData& content, const ESM::RefId& refId, ESM::RecNameInts type);

    /// The texture a blend map's `index` names in `plugin`, or null where the pair names nothing.
    const VFS::Path::Normalized* getLandTexture(const EsmData& content, std::uint32_t index, int plugin);

    ESM::Variant getGameSetting(const std::vector<ESM::GameSetting>& records, std::string_view id);
}

#endif

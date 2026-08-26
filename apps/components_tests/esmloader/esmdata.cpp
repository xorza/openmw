#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/variant.hpp>
#include <components/esmloader/esmdata.hpp>

#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace
{
    using namespace testing;
    using namespace EsmLoader;

    struct Params
    {
        std::string mRefId;
        ESM::RecNameInts mType;
        VFS::Path::NormalizedView mResult;
        std::function<void(EsmData&)> mPushBack;
    };

    struct EsmLoaderGetModelTest : TestWithParam<Params>
    {
    };

    TEST_P(EsmLoaderGetModelTest, shouldReturnFoundModelName)
    {
        EsmData data;
        GetParam().mPushBack(data);
        EXPECT_EQ(EsmLoader::getModel(data, ESM::RefId::stringRefId(GetParam().mRefId), GetParam().mType),
            GetParam().mResult);
    }

    template <class T>
    struct PushBack
    {
        std::string mId;
        std::string mModel;

        void operator()(EsmData& esmData) const
        {
            T value;
            value.mId = ESM::RefId::stringRefId(mId);
            value.mModel = mModel;
            esmData.get<T>().push_back(std::move(value));
        }
    };

    const std::array params = {
        Params{ "acti_ref_id", ESM::REC_ACTI, VFS::Path::NormalizedView("acti_model"),
            PushBack<ESM::Activator>{ "acti_ref_id", "acti_model" } },
        Params{ "cont_ref_id", ESM::REC_CONT, VFS::Path::NormalizedView("cont_model"),
            PushBack<ESM::Container>{ "cont_ref_id", "cont_model" } },
        Params{ "door_ref_id", ESM::REC_DOOR, VFS::Path::NormalizedView("door_model"),
            PushBack<ESM::Door>{ "door_ref_id", "door_model" } },
        Params{ "static_ref_id", ESM::REC_STAT, VFS::Path::NormalizedView("static_model"),
            PushBack<ESM::Static>{ "static_ref_id", "static_model" } },
        Params{ "acti_ref_id_a", ESM::REC_ACTI, {}, PushBack<ESM::Activator>{ "acti_ref_id_z", "acti_model" } },
        Params{ "cont_ref_id_a", ESM::REC_CONT, {}, PushBack<ESM::Container>{ "cont_ref_id_z", "cont_model" } },
        Params{ "door_ref_id_a", ESM::REC_DOOR, {}, PushBack<ESM::Door>{ "door_ref_id_z", "door_model" } },
        Params{ "static_ref_id_a", ESM::REC_STAT, {}, PushBack<ESM::Static>{ "static_ref_id_z", "static_model" } },
        Params{ "acti_ref_id_z", ESM::REC_ACTI, {}, PushBack<ESM::Activator>{ "acti_ref_id_a", "acti_model" } },
        Params{ "cont_ref_id_z", ESM::REC_CONT, {}, PushBack<ESM::Container>{ "cont_ref_id_a", "cont_model" } },
        Params{ "door_ref_id_z", ESM::REC_DOOR, {}, PushBack<ESM::Door>{ "door_ref_id_a", "door_model" } },
        Params{ "static_ref_id_z", ESM::REC_STAT, {}, PushBack<ESM::Static>{ "static_ref_id_a", "static_model" } },
        // The types the widening added, which the four above cannot speak for: a lookup that only
        // knew the collision-bearing records would answer these with nothing and look right doing it.
        Params{ "light_ref_id", ESM::REC_LIGH, VFS::Path::NormalizedView("light_model"),
            PushBack<ESM::Light>{ "light_ref_id", "light_model" } },
        Params{ "book_ref_id", ESM::REC_BOOK, VFS::Path::NormalizedView("book_model"),
            PushBack<ESM::Book>{ "book_ref_id", "book_model" } },
        Params{ "misc_ref_id", ESM::REC_MISC, VFS::Path::NormalizedView("misc_model"),
            PushBack<ESM::Miscellaneous>{ "misc_ref_id", "misc_model" } },
        // The right id in the wrong table stays unfound: the type picks the table before the id is
        // looked for, so a light and a book sharing a name are still two different records.
        Params{ "light_ref_id", ESM::REC_BOOK, {}, PushBack<ESM::Light>{ "light_ref_id", "light_model" } },
        Params{ "ref_id", ESM::REC_STAT, {}, [](EsmData&) {} },
        // A record type the list deliberately leaves out. An NPC names no model of its own.
        Params{ "ref_id", ESM::REC_NPC_, {}, [](EsmData&) {} },
    };

    INSTANTIATE_TEST_SUITE_P(Params, EsmLoaderGetModelTest, ValuesIn(params));

    LandTextureRecord makeLandTexture(std::string_view id, std::uint32_t index, const char* texture, int plugin)
    {
        return LandTextureRecord{
            .mId = ESM::RefId::stringRefId(id),
            .mIndex = index,
            .mTexture = VFS::Path::Normalized(texture),
            .mPlugin = plugin,
            .mDeleted = false,
        };
    }

    /// The same index means different textures in different plugins, and both stay reachable.
    ///
    /// A cell's blend map indexes into the texture list of the file that wrote the cell, so a pair
    /// keyed on the index alone would have the later plugin silently repaint the earlier one's
    /// ground.
    TEST(EsmLoaderLandTextureTest, anIndexIsResolvedWithinThePluginThatClaimedIt)
    {
        const std::array records{
            makeLandTexture("sand", 0, "tx_sand.dds", 0),
            makeLandTexture("rock", 1, "tx_rock.dds", 0),
            makeLandTexture("snow", 0, "tx_snow.dds", 1),
        };

        EsmData data;
        data.mLandTextures = prepareLandTextures(records);

        ASSERT_EQ(data.mLandTextures.size(), 3);
        ASSERT_NE(getLandTexture(data, 0, 0), nullptr);
        EXPECT_EQ(*getLandTexture(data, 0, 0), VFS::Path::NormalizedView("tx_sand.dds"));
        ASSERT_NE(getLandTexture(data, 1, 0), nullptr);
        EXPECT_EQ(*getLandTexture(data, 1, 0), VFS::Path::NormalizedView("tx_rock.dds"));
        ASSERT_NE(getLandTexture(data, 0, 1), nullptr);
        EXPECT_EQ(*getLandTexture(data, 0, 1), VFS::Path::NormalizedView("tx_snow.dds"));

        EXPECT_EQ(getLandTexture(data, 1, 1), nullptr);
        EXPECT_EQ(getLandTexture(data, 7, 0), nullptr);
    }

    /// The two merge rules, which point opposite ways and are easy to get backwards.
    ///
    /// Redefining an **id** replaces the texture everywhere the id is reached, which is how a
    /// replacer plugin changes what sand looks like. Claiming a **(plugin, index)** pair a second
    /// time does nothing, because the pair names a slot in one file's own list.
    TEST(EsmLoaderLandTextureTest, aRedefinedIdRepaintsEveryIndexThatReachesItAndAReusedPairIsIgnored)
    {
        const std::array records{
            makeLandTexture("sand", 0, "tx_sand.dds", 0),
            // A later file gives the same id a different texture, and never mentions an index of
            // its own: the plugin-zero cell that names index 0 must now get the new one.
            makeLandTexture("sand", 3, "tx_ash.dds", 1),
            // And a second claim on a pair plugin zero already used, which must not take.
            makeLandTexture("rock", 0, "tx_rock.dds", 0),
        };

        EsmData data;
        data.mLandTextures = prepareLandTextures(records);

        ASSERT_NE(getLandTexture(data, 0, 0), nullptr);
        EXPECT_EQ(*getLandTexture(data, 0, 0), VFS::Path::NormalizedView("tx_ash.dds"));
        ASSERT_NE(getLandTexture(data, 3, 1), nullptr);
        EXPECT_EQ(*getLandTexture(data, 3, 1), VFS::Path::NormalizedView("tx_ash.dds"));
    }

    /// A deleted record contributes neither a texture nor a claim on its pair.
    TEST(EsmLoaderLandTextureTest, aDeletedRecordLeavesNothingBehind)
    {
        std::array records{
            makeLandTexture("sand", 0, "tx_sand.dds", 0),
            makeLandTexture("rock", 1, "tx_rock.dds", 0),
        };
        records[0].mDeleted = true;

        EsmData data;
        data.mLandTextures = prepareLandTextures(records);

        EXPECT_EQ(data.mLandTextures.size(), 1);
        EXPECT_EQ(getLandTexture(data, 0, 0), nullptr);
        EXPECT_NE(getLandTexture(data, 1, 0), nullptr);
    }

    TEST(EsmLoaderGetGameSettingTest, shouldReturnFoundValue)
    {
        std::vector<ESM::GameSetting> settings;
        ESM::GameSetting setting;
        setting.mId = ESM::RefId::stringRefId("setting");
        setting.mValue = ESM::Variant(42);
        setting.mRecordFlags = 0;
        settings.push_back(setting);
        EXPECT_EQ(EsmLoader::getGameSetting(settings, "setting"), ESM::Variant(42));
    }

    TEST(EsmLoaderGetGameSettingTest, shouldThrowExceptionWhenNotFound)
    {
        const std::vector<ESM::GameSetting> settings;
        EXPECT_THROW(EsmLoader::getGameSetting(settings, "setting"), std::runtime_error);
    }
}

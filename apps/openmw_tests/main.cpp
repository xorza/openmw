#include <components/debug/debugging.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/strings/conversion.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/values.hpp>
#include <components/testing/util.hpp>

#include <gtest/gtest.h>

#include "apps/components_tests/fallbackseed.hpp"

#include <filesystem>

int main(int argc, char* argv[])
{
    Log::sMinDebugLevel = Debug::getDebugLevel();

    const std::filesystem::path settingsDefaultPath = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "files"
        / Misc::StringUtils::stringToU8String("settings-default.cfg");

    Settings::SettingsFileParser parser;
    parser.loadSettingsFile(settingsDefaultPath, Settings::Manager::mDefaultSettings);

    Settings::StaticValues::initDefaults();

    Settings::Manager::mUserSettings = Settings::Manager::mDefaultSettings;

    Settings::StaticValues::init();

    // **Before any test runs, because `Fallback::Map::init` keeps whichever value arrives first.**
    // The sky refuses to answer an hour before it has been told when the day starts, and the same
    // seed this fork's component tests stand on is what tells it. `fallbackSeed` says the rest.
    Fallback::Map::init(TestingOpenMW::fallbackSeed());

    testing::InitGoogleTest(&argc, argv);

    const int result = RUN_ALL_TESTS();
    if (result == 0)
        std::filesystem::remove_all(TestingOpenMW::outputDir());
    return result;
}

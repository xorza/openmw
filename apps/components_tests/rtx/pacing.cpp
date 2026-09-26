#include <array>
#include <limits>
#include <optional>

#include <gtest/gtest.h>

#include <components/rtx/menu.hpp>
#include <components/rtx/pacing.hpp>

namespace Rtx
{
    namespace
    {
        /// The three spellings a settings file writes, which a renamed mode would stop reading.
        TEST(RtxPacingTest, theModesAreSpelledAsASettingsFileWritesThemAndTheMenusListThemInOrder)
        {
            EXPECT_EQ(sLatencyModeNames.named("off"), LatencyMode::Off);
            EXPECT_EQ(sLatencyModeNames.named("on"), LatencyMode::On);
            EXPECT_EQ(sLatencyModeNames.named("boost"), LatencyMode::Boost);

            // The menus list every mode in that order, and a position past the end or a name off the
            // list answers nothing rather than a mode.
            EXPECT_EQ(menuIndex(sLatencyMenu, "off"), 0u);
            EXPECT_EQ(menuIndex(sLatencyMenu, "boost"), 2u);
            EXPECT_EQ(menuIndex(sLatencyMenu, "fast"), std::nullopt);
            EXPECT_EQ(menuName(sLatencyMenu, 1), "on");
            EXPECT_EQ(menuName(sLatencyMenu, 3), std::nullopt);

            // A menu's labels are held to that order: in it they pass, and with two swapped, or one
            // spelled otherwise, they do not.
            constexpr std::array<MenuLabel, 3> labels{ { { "off", "Off" }, { "on", "On" }, { "boost", "Boost" } } };
            constexpr std::array<MenuLabel, 3> swapped{ { { "on", "On" }, { "off", "Off" }, { "boost", "Boost" } } };
            constexpr std::array<MenuLabel, 3> misspelled{ { { "off", "Off" }, { "on", "On" }, { "Boost", "Boost" } } };
            EXPECT_TRUE(followsMenu(labels, sLatencyMenu));
            EXPECT_FALSE(followsMenu(swapped, sLatencyMenu));
            EXPECT_FALSE(followsMenu(misspelled, sLatencyMenu));
        }

        /// The interval the sleep enforces is the limit turned round, to the nearest microsecond,
        /// and nought for no limit however it is spelled.
        TEST(RtxPacingTest, theFrameRateLimitBecomesTheSleepsInterval)
        {
            EXPECT_EQ(minimumIntervalOf(0.0f), 0u);
            EXPECT_EQ(minimumIntervalOf(-60.0f), 0u);
            EXPECT_EQ(minimumIntervalOf(100.0f), 10000u);
            EXPECT_EQ(minimumIntervalOf(120.0f), 8333u) << "1000000 / 120 = 8333.33, to the nearest";
            EXPECT_EQ(minimumIntervalOf(144.0f), 6944u) << "6944.44";
            EXPECT_EQ(minimumIntervalOf(240.0f), 4167u) << "4166.67";

            // 1000000 / 2^-10 = 1024000000 fits the field, 1000000 / 1e-5 = 1e11 is past its
            // 4294967295, and a limit past every frame rate asks for no wait at all.
            EXPECT_EQ(minimumIntervalOf(0.0009765625f), 1024000000u);
            EXPECT_EQ(minimumIntervalOf(1e-5f), 4294967295u);
            EXPECT_EQ(minimumIntervalOf(std::numeric_limits<float>::infinity()), 0u);
        }
    }
}

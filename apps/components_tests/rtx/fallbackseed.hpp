#pragma once

#include <initializer_list>
#include <map>
#include <string>
#include <string_view>

#include <components/rtx/error.hpp>

namespace Rtx::Testing
{
    /// The three keys the day's phases are read from — `Sky::TimeOfDaySettings::fromFallback`.
    ///
    /// **`Fallback::Map::init` keeps whichever value arrives first**, so two tests planting one key
    /// have to plant it the same — which is why every seed in this header is here rather than in the
    /// test that wants it. A suite that has already opened the real installation keeps its numbers
    /// and these are ignored, so an expectation has to be written against what the code says rather
    /// than against a value planted here.
    ///
    /// **`Sky::TimeOfDaySettings::shared` refuses a day that ends before it starts**, so anything
    /// that asks the sky about an hour has to plant these first, whatever it is about.
    inline std::map<std::string, std::string> clockSeed()
    {
        return {
            { "Weather_Sunrise_Time", "6" },
            { "Weather_Sunset_Time", "18" },
            { "Weather_Sunset_Duration", "2" },
        };
    }

    /// Every fallback key `Rtx::requireWeather` demands of each of `weathers`, plus the clock keys
    /// their ramps are read against, as `Fallback::Map::init` takes them.
    ///
    /// **A weather short of one key is refused by name**, so a test that reads a weather has to
    /// plant all of them — one that planted only what its assertions read would stand on whichever
    /// other test happened to run first.
    ///
    /// **Morrowind's own numbers, except Clear's two fog depths.** The game ships one depth for both
    /// day and night, and a test that a night is foggier than a day needs the two to differ.
    ///
    /// Refuses a weather it has no numbers for, so a caller cannot quietly plant nothing.
    inline std::map<std::string, std::string> weatherSeed(std::initializer_list<std::string_view> weathers)
    {
        std::map<std::string, std::string> seed = clockSeed();

        for (const std::string_view weather : weathers)
        {
            const auto plant = [&seed, weather](std::string_view key, std::string_view value) {
                seed.emplace("Weather_" + std::string(weather) + "_" + std::string(key), value);
            };

            if (weather == "Clear")
            {
                plant("Sky_Sunrise_Color", "117,141,164");
                plant("Sky_Day_Color", "095,135,203");
                plant("Sky_Sunset_Color", "056,089,129");
                plant("Sky_Night_Color", "009,010,011");
                plant("Fog_Sunrise_Color", "255,189,157");
                plant("Fog_Day_Color", "206,227,255");
                plant("Fog_Sunset_Color", "255,189,157");
                plant("Fog_Night_Color", "009,010,011");
                plant("Ambient_Sunrise_Color", "047,066,096");
                plant("Ambient_Day_Color", "137,140,160");
                plant("Ambient_Sunset_Color", "068,075,096");
                plant("Ambient_Night_Color", "032,035,042");

                // The night value being the blue one is what a sun disc is tested not to be painted
                // with.
                plant("Sun_Sunrise_Color", "242,159,119");
                plant("Sun_Day_Color", "255,252,238");
                plant("Sun_Sunset_Color", "255,114,079");
                plant("Sun_Night_Color", "059,097,176");

                plant("Sun_Disc_Sunset_Color", "255,189,157");
                plant("Cloud_Texture", "Tx_Sky_Clear.dds");

                // The two the game ships equal, at `.69` apiece. Apart here, so that a night deeper
                // than a day is something a test can ask about at all.
                plant("Land_Fog_Day_Depth", "0.4");
                plant("Land_Fog_Night_Depth", "0.8");

                plant("Glare_View", "1");
                plant("Wind_Speed", "0.3");
                plant("Cloud_Speed", "1.25");
                plant("Clouds_Maximum_Percent", "1.0");
            }
            else if (weather == "Overcast")
            {
                plant("Sky_Sunrise_Color", "091,099,106");
                plant("Sky_Day_Color", "143,146,149");
                plant("Sky_Sunset_Color", "108,115,121");
                plant("Sky_Night_Color", "019,022,025");
                plant("Fog_Sunrise_Color", "091,099,106");
                plant("Fog_Day_Color", "143,146,149");
                plant("Fog_Sunset_Color", "108,115,121");
                plant("Fog_Night_Color", "019,022,025");
                plant("Ambient_Sunrise_Color", "084,088,092");
                plant("Ambient_Day_Color", "093,096,105");
                plant("Ambient_Sunset_Color", "083,077,075");
                plant("Ambient_Night_Color", "057,060,066");
                plant("Sun_Sunrise_Color", "087,125,163");
                plant("Sun_Day_Color", "163,169,183");
                plant("Sun_Sunset_Color", "085,103,157");
                plant("Sun_Night_Color", "032,054,100");
                plant("Sun_Disc_Sunset_Color", "128,128,128");
                plant("Cloud_Texture", "Tx_Sky_Overcast.dds");
                plant("Land_Fog_Day_Depth", "0.9");
                plant("Land_Fog_Night_Depth", "0.9");
                plant("Glare_View", "0");
                plant("Wind_Speed", "0.2");
                plant("Cloud_Speed", "1.5");
                plant("Clouds_Maximum_Percent", "1.0");
            }
            else
                throw Error("no seed for weather \"" + std::string(weather) + "\"");
        }

        return seed;
    }

    /// Morrowind's own `[Moons]`, as the ini ships it.
    ///
    /// **A test that places a moon has to plant these, whatever it is about.** `subtendedBy` refuses
    /// a moon of no size, and `Fallback::Map` answers a key nobody planted with a silent nought — so
    /// a test that reaches a moon without these takes the whole suite down rather than its own case.
    ///
    /// Every value is the ini's, and every one matches the default OpenMW ships **except the two
    /// sizes**, where the ini says 94 and 40 against OpenMW's 55 and 20. That is why nothing asserts
    /// a size against a number, and why everything that turns on the arc can: the speeds, the
    /// increments and the fade angles agree between the two.
    inline std::map<std::string, std::string> moonSeed()
    {
        return {
            { "Moons_Masser_Size", "94" },
            { "Moons_Masser_Fade_In_Start", "14" },
            { "Moons_Masser_Fade_In_Finish", "15" },
            { "Moons_Masser_Fade_Out_Start", "7" },
            { "Moons_Masser_Fade_Out_Finish", "10" },
            { "Moons_Masser_Axis_Offset", "35" },
            { "Moons_Masser_Speed", "0.5" },
            { "Moons_Masser_Daily_Increment", "1" },
            { "Moons_Masser_Fade_Start_Angle", "50" },
            { "Moons_Masser_Fade_End_Angle", "40" },
            { "Moons_Masser_Moon_Shadow_Early_Fade_Angle", "0.5" },

            { "Moons_Secunda_Size", "40" },
            { "Moons_Secunda_Fade_In_Start", "14" },
            { "Moons_Secunda_Fade_In_Finish", "15" },
            { "Moons_Secunda_Fade_Out_Start", "7" },
            { "Moons_Secunda_Fade_Out_Finish", "10" },
            { "Moons_Secunda_Axis_Offset", "50" },
            { "Moons_Secunda_Speed", "0.6" },
            { "Moons_Secunda_Daily_Increment", "1.2" },
            { "Moons_Secunda_Fade_Start_Angle", "50" },
            { "Moons_Secunda_Fade_End_Angle", "40" },
            { "Moons_Secunda_Moon_Shadow_Early_Fade_Angle", "0.5" },
        };
    }
}

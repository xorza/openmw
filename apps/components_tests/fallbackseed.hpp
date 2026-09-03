#pragma once

#include <map>
#include <string>

namespace TestingOpenMW
{
    /// Every fallback key this binary's tests read, planted once before any of them runs.
    ///
    /// **`Fallback::Map::init` keeps whichever value arrives first**, so a key two tests both wanted
    /// used to belong to whichever ran first — and a test that planted only what its own assertions
    /// read stood on whatever the one before it happened to leave. Planting the lot in `main` is what
    /// makes the order the suite runs in stop deciding anything.
    ///
    /// **Not what a test asserts against.** An expectation is written against what the code says,
    /// not against a number here, because the same code has to answer for a real installation's
    /// values too.
    ///
    /// **The installation still supplies the rest.** A test that opens the real Morrowind merges its
    /// whole fallback map in, and everything below is already planted — so the keys here are pinned
    /// and every other key is the installation's.
    ///
    /// Morrowind's own numbers throughout, with one exception marked below.
    inline std::map<std::string, std::string> fallbackSeed()
    {
        return {
            // The day's phases. `Sky::TimeOfDaySettings::shared` refuses a day that ends before it
            // starts, so anything asking the sky about an hour needs these.
            { "Weather_Sunrise_Time", "6" },
            { "Weather_Sunrise_Duration", "2" },
            { "Weather_Sunset_Time", "18" },
            { "Weather_Sunset_Duration", "2" },

            // **The window each quantity crosses dawn and dusk over, which is its own.** A name
            // `TimeOfDaySettings::getSetting` does not hold answers an hour either side, which is a
            // usable window and so cannot be told from a recorded one — an hour that fell inside it
            // would then blend where a seeded schedule reads a phase outright.
            { "Weather_Sky_Pre-Sunrise_Time", ".5" },
            { "Weather_Sky_Post-Sunrise_Time", "1" },
            { "Weather_Sky_Pre-Sunset_Time", "1.5" },
            { "Weather_Sky_Post-Sunset_Time", ".5" },
            { "Weather_Ambient_Pre-Sunrise_Time", ".5" },
            { "Weather_Ambient_Post-Sunrise_Time", "2" },
            { "Weather_Ambient_Pre-Sunset_Time", "1" },
            { "Weather_Ambient_Post-Sunset_Time", "1.25" },
            { "Weather_Fog_Pre-Sunrise_Time", ".5" },
            { "Weather_Fog_Post-Sunrise_Time", "1" },
            { "Weather_Fog_Pre-Sunset_Time", "2" },
            { "Weather_Fog_Post-Sunset_Time", "1" },
            { "Weather_Sun_Pre-Sunrise_Time", "0" },
            { "Weather_Sun_Post-Sunrise_Time", "0" },
            { "Weather_Sun_Pre-Sunset_Time", "1" },
            { "Weather_Sun_Post-Sunset_Time", "1.25" },

            // The stars' own window is derived from these three.
            { "Weather_Stars_Post-Sunset_Start", "1" },
            { "Weather_Stars_Pre-Sunrise_Finish", "2" },
            { "Weather_Stars_Fading_Duration", "2" },

            // Clear and Overcast, whole. `Rtx::requireWeather` refuses a weather short of one key by
            // name, so a partial weather is no weather at all.
            { "Weather_Clear_Sky_Sunrise_Color", "117,141,164" },
            { "Weather_Clear_Sky_Day_Color", "095,135,203" },
            { "Weather_Clear_Sky_Sunset_Color", "056,089,129" },
            { "Weather_Clear_Sky_Night_Color", "009,010,011" },
            { "Weather_Clear_Fog_Sunrise_Color", "255,189,157" },
            { "Weather_Clear_Fog_Day_Color", "206,227,255" },
            { "Weather_Clear_Fog_Sunset_Color", "255,189,157" },
            { "Weather_Clear_Fog_Night_Color", "009,010,011" },
            { "Weather_Clear_Ambient_Sunrise_Color", "047,066,096" },
            { "Weather_Clear_Ambient_Day_Color", "137,140,160" },
            { "Weather_Clear_Ambient_Sunset_Color", "068,075,096" },
            { "Weather_Clear_Ambient_Night_Color", "032,035,042" },

            // The night value being the blue one is what a sun disc is tested not to be painted with.
            { "Weather_Clear_Sun_Sunrise_Color", "242,159,119" },
            { "Weather_Clear_Sun_Day_Color", "255,252,238" },
            { "Weather_Clear_Sun_Sunset_Color", "255,114,079" },
            { "Weather_Clear_Sun_Night_Color", "059,097,176" },

            { "Weather_Clear_Sun_Disc_Sunset_Color", "255,189,157" },
            { "Weather_Clear_Cloud_Texture", "Tx_Sky_Clear.dds" },

            // **The one exception.** The game ships `.69` for both of Clear's depths, and a night
            // that is foggier than a day is something a test can only ask about where the two differ.
            { "Weather_Clear_Land_Fog_Day_Depth", "0.4" },
            { "Weather_Clear_Land_Fog_Night_Depth", "0.8" },

            { "Weather_Clear_Glare_View", "1" },
            { "Weather_Clear_Wind_Speed", "0.3" },
            { "Weather_Clear_Cloud_Speed", "1.25" },
            { "Weather_Clear_Clouds_Maximum_Percent", "1.0" },

            { "Weather_Overcast_Sky_Sunrise_Color", "091,099,106" },
            { "Weather_Overcast_Sky_Day_Color", "143,146,149" },
            { "Weather_Overcast_Sky_Sunset_Color", "108,115,121" },
            { "Weather_Overcast_Sky_Night_Color", "019,022,025" },
            { "Weather_Overcast_Fog_Sunrise_Color", "091,099,106" },
            { "Weather_Overcast_Fog_Day_Color", "143,146,149" },
            { "Weather_Overcast_Fog_Sunset_Color", "108,115,121" },
            { "Weather_Overcast_Fog_Night_Color", "019,022,025" },
            { "Weather_Overcast_Ambient_Sunrise_Color", "084,088,092" },
            { "Weather_Overcast_Ambient_Day_Color", "093,096,105" },
            { "Weather_Overcast_Ambient_Sunset_Color", "083,077,075" },
            { "Weather_Overcast_Ambient_Night_Color", "057,060,066" },
            { "Weather_Overcast_Sun_Sunrise_Color", "087,125,163" },
            { "Weather_Overcast_Sun_Day_Color", "163,169,183" },
            { "Weather_Overcast_Sun_Sunset_Color", "085,103,157" },
            { "Weather_Overcast_Sun_Night_Color", "032,054,100" },
            { "Weather_Overcast_Sun_Disc_Sunset_Color", "128,128,128" },
            { "Weather_Overcast_Cloud_Texture", "Tx_Sky_Overcast.dds" },
            { "Weather_Overcast_Land_Fog_Day_Depth", "0.9" },
            { "Weather_Overcast_Land_Fog_Night_Depth", "0.9" },
            { "Weather_Overcast_Glare_View", "0" },
            { "Weather_Overcast_Wind_Speed", "0.2" },
            { "Weather_Overcast_Cloud_Speed", "1.5" },
            { "Weather_Overcast_Clouds_Maximum_Percent", "1.0" },

            // **Drizzle is deliberately absent**, because a weather the configuration left out is
            // refused by name and there is a test that it is.

            // What a weather drops, for the three the downpour tests ask about, and one more wind so
            // that two weathers cannot be read as one number.
            { "Weather_Clear_Using_Precip", "0" },
            { "Weather_Rain_Using_Precip", "1" },
            { "Weather_Thunderstorm_Using_Precip", "1" },
            { "Weather_Ashstorm_Wind_Speed", "0.8" },

            // The `[Moons]`, as the ini ships them. `Rtx::moonbuilder` refuses a moon with no size
            // or no speed, so anything that places one needs these.
            //
            // Every value matches the default OpenMW ships **except the two sizes**, where the ini
            // says 94 and 40 against OpenMW's 55 and 20 — which is why the moon tests assert the law
            // an arc follows rather than a number a size gives.
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

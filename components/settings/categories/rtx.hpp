#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RTX_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RTX_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <components/rtx/upscale.hpp>
#include <components/settings/settingvalue.hpp>

namespace Settings
{
    /// Whether this binary was built with the ray tracing renderer. The settings below exist either
    /// way, so a configuration file survives moving between builds.
#ifdef OPENMW_RTX
    inline constexpr bool sRayTracingBuilt = true;
#else
    inline constexpr bool sRayTracingBuilt = false;
#endif

    /// The experimental ray tracing renderer. Which renderer draws is read once, before the window
    /// exists; the rest are read as they are wanted.
    struct RTXCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<bool> mEnabled{ mIndex, "RTX", "enabled" };

        /// How far out from the eye the world is built, in cells, read every frame by the world
        /// mirror so the menu's slider moves the rings, the air and the map at once. How much world
        /// exists is a property of the structure rays are cast against and not of the camera, which
        /// is what `viewing distance` is about; the air is tuned to it as well as the ground
        /// (`Rtx::distantLandReach`).
        SettingValue<float> mDistantLandCells{ mIndex, "RTX", "distant land cells" };

        /// How hard DLSS Ray Reconstruction works, or `off`: a name `Rtx::sUpscaleNames` refuses
        /// rather than defaults. Changing it rebuilds every target, and a machine that cannot reach
        /// the mode keeps the one it had and says so in the log.
        SettingValue<std::string> mUpscale{ mIndex, "RTX", "upscale" };

        /// The modes the launcher and the settings window offer, in the order both list them,
        /// spelled as `mUpscale` takes them: fewest pixels traced first, every pixel last. `off` is
        /// not among them: Ray Reconstruction is the renderer's denoiser, so a menu that offered
        /// it would offer a worse picture as a speed setting. Derived from `Rtx::sUpscaleNames`,
        /// the one list of the spellings, so a mode added there reaches both menus.
        static constexpr std::array<std::string_view, Rtx::sUpscaleNames.mNames.size() - 1> sUpscaleMenu = [] {
            std::array<std::string_view, Rtx::sUpscaleNames.mNames.size() - 1> offered{};
            std::size_t at = 0;
            for (const auto& [mode, spelling] : Rtx::sUpscaleNames.mNames)
                if (mode != Rtx::Upscale::Off)
                    offered[at++] = spelling;

            return offered;
        }();

        /// Where the mode `name` spells sits in that menu, or nothing for one it does not offer.
        static std::optional<std::size_t> upscaleMenuIndex(std::string_view name)
        {
            const auto* found = std::find(sUpscaleMenu.begin(), sUpscaleMenu.end(), name);
            if (found == sUpscaleMenu.end())
                return std::nullopt;

            return static_cast<std::size_t>(found - sUpscaleMenu.begin());
        }

        /// The mode at `index` of that menu, or nothing where the menu is shorter than that — asked
        /// rather than indexed, because the list of entries lives in a layout file, and a menu with
        /// an entry the list has no mode for would otherwise read past the end of it.
        static std::optional<std::string_view> upscaleMenuName(std::size_t index)
        {
            if (index >= sUpscaleMenu.size())
                return std::nullopt;

            return sUpscaleMenu[index];
        }

        /// Which Ray Reconstruction network runs, as `Rtx::sPresetNames` spells them.
        SettingValue<std::string> mPreset{ mIndex, "RTX", "preset" };
    };
}

#endif

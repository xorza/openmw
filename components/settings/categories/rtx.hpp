#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RTX_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RTX_H

#include <string>

#include <components/settings/sanitizerimpl.hpp>
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

        /// The most cells `distant land cells` takes: a bound on how much world a frame is asked to
        /// stand, and so on what the ring's ask costs. Nought stays nought, which hands the reach
        /// back to `viewing distance`.
        static constexpr float sMaxDistantLandCells = 10.0f;

        /// The fewest the two menus offer. A file may name fewer, and the launcher leaves such a
        /// value as it found it.
        static constexpr float sMinDistantLandCellsInMenu = 4.0f;

        /// How far out from the eye the world is built, in cells: handed to the world mirror where
        /// the renderer is made and again when the menu moves it, so the rings, the air and the map
        /// follow one number. How much world exists is a property of the structure rays are cast
        /// against and not of the camera, which is what `viewing distance` is about; the air is
        /// tuned to it as well as the ground (`Rtx::distantLandReach`).
        SettingValue<float> mDistantLandCells{ mIndex, "RTX", "distant land cells",
            makeClampSanitizerFloat(0.0f, sMaxDistantLandCells) };

        /// How hard DLSS Ray Reconstruction works, or `off`: a name `Rtx::sUpscaleNames` refuses
        /// rather than defaults. Changing it rebuilds every target, and a machine that cannot reach
        /// the mode keeps the one it had and says so in the log. `Rtx::sUpscaleMenu` is what the
        /// launcher and the settings window offer of it.
        SettingValue<std::string> mUpscale{ mIndex, "RTX", "upscale" };

        /// Which Ray Reconstruction network runs, as `Rtx::sPresetNames` spells them.
        SettingValue<std::string> mPreset{ mIndex, "RTX", "preset" };

        /// How the driver paces the frame, as `Rtx::sLatencyModeNames` spells the modes: off, on
        /// or boost. Read where the renderer is made and again on a change, like the vertical
        /// sync; a machine whose driver paces nothing keeps the setting and does nothing with it.
        SettingValue<std::string> mReflex{ mIndex, "RTX", "reflex" };

        /// Whether a left click marks its frame for the driver's latency analyser, which draws a
        /// square on it. A measurement aid and never on by default.
        SettingValue<bool> mReflexFlash{ mIndex, "RTX", "reflex flash" };
    };
}

#endif

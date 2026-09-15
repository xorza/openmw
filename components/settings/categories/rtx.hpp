#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RTX_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RTX_H

#include <string>

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

        /// Which Ray Reconstruction network runs, as `Rtx::sPresetNames` spells them.
        SettingValue<std::string> mPreset{ mIndex, "RTX", "preset" };

        /// How long every frame stands for, in seconds, or nought to time each one. What makes two
        /// runs of one build the same run: everything the world animates steps by it, so ten seconds
        /// of world is six hundred frames on every machine. Read once into the one `Rtx::FrameClock`
        /// a renderer keeps. Nought for anybody playing; the rasterizer times every frame regardless.
        SettingValue<float> mFixedStep{ mIndex, "RTX", "fixed step" };

        /// A run to make instead of a session to play, as `Rtx::readSpec` spells one — `600`,
        /// `10s`, `10s:2s`, `10s:2s@12000`: how long, how much of it warms up, how fast to fly.
        /// Where to stand is a savegame's business. Empty is a session somebody is playing.
        SettingValue<std::string> mSession{ mIndex, "RTX", "session" };

        /// How much of the lighting painted into each vanilla texture to divide back out, from
        /// nought to one. Nought shows the textures as they were drawn, which is the A/B that says
        /// what the recovery did.
        SettingValue<float> mDelight{ mIndex, "RTX", "delight" };

        /// Draw the albedo the materials recovered instead of tracing the frame.
        SettingValue<bool> mShowAlbedo{ mIndex, "RTX", "show albedo" };

        /// Whether the denoiser runs over the indirect light. Off shows the raw bounce, and is what
        /// a reference is made with. Ignored while the upscaler is denoising for itself.
        SettingValue<bool> mFilter{ mIndex, "RTX", "filter" };

        /// What to scale the frame by before the display curve, or nought to measure it off the
        /// frame. A picture wants it measured; a reference wants it held still.
        SettingValue<float> mExposure{ mIndex, "RTX", "exposure" };

        /// Whether each frame samples a different point inside its pixel. Only worth anything to
        /// something putting several frames together, and forced on while anything upscales.
        SettingValue<bool> mJitter{ mIndex, "RTX", "jitter" };

        /// Whether the trace also counts the see-through surfaces each primary ray crosses. Off,
        /// because it is a second traversal a pixel.
        SettingValue<bool> mCountCrossings{ mIndex, "RTX", "count crossings" };
    };
}

#endif

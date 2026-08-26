#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RTX_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RTX_H

#include <string>

#include <components/settings/settingvalue.hpp>

namespace Settings
{
    /// The experimental ray tracing renderer.
    ///
    /// These are read once, at startup: the renderer decides how the window is created and what
    /// draws into it, so there is nothing meaningful to change while a frame is in flight. The
    /// settings exist whether or not the renderer was compiled in, so that a configuration file
    /// survives moving between builds.
    struct RTXCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<bool> mEnabled{ mIndex, "RTX", "enabled" };

        /// How far out from the eye the world is built, in cells.
        ///
        /// **How much world exists, which is a property of the structure rays are cast against and
        /// not of the camera.** `viewing distance` is the rasterizer's fog-and-visibility knob and
        /// means something else: at 7168 against a cell of 8192 it barely leaves the active grid.
        ///
        /// The air is tuned to this as well as the ground, because fog measured in one distance over
        /// a world built to another is a world you cannot see — see `Rtx::distantLandReach`.
        SettingValue<float> mDistantLandCells{ mIndex, "RTX", "distant land cells" };

        /// Loads the Vulkan validation layers and refuses to start on anything they object to.
        ///
        /// **No control offers this, on purpose.** It is a developer's diagnostic: it costs a large
        /// part of the frame rate, it needs layers that are not on a player's machine, and what it
        /// does when it finds something is stop. It is reached by editing `settings.cfg`, which is
        /// the audience it has; `openmw-rtxtool --validation` is the same switch for the harness and
        /// is on there by default outside a Release build.
        SettingValue<bool> mValidation{ mIndex, "RTX", "validation" };

        /// How hard DLSS Ray Reconstruction works, or `off` for none of it.
        ///
        /// A name rather than a number, and unrecognised is refused rather than defaulted — see
        /// `Rtx::upscaleNamed`.
        SettingValue<std::string> mUpscale{ mIndex, "RTX", "upscale" };

        /// Which Ray Reconstruction network runs, where one runs at all.
        ///
        /// A name rather than a number, refused rather than defaulted when unrecognised — see
        /// `Rtx::presetNamed`. Ray Reconstruction keeps its own presets, which are not
        /// super-resolution's.
        SettingValue<std::string> mPreset{ mIndex, "RTX", "preset" };
    };
}

#endif

#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RTX_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_RTX_H

#include <string>

#include <components/settings/settingvalue.hpp>

namespace Settings
{
    /// The experimental ray tracing renderer.
    ///
    /// **Which renderer draws is read once and the rest are read as they are wanted.** The choice
    /// decides how the window is created and what draws into it, so it cannot be made again while a
    /// frame is in flight; how much world is built and how hard the upscaler works are questions the
    /// renderer asks again, and both are offered in the game's own menu.
    ///
    /// The settings exist whether or not the renderer was compiled in, so that a configuration file
    /// survives moving between builds.
    struct RTXCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<bool> mEnabled{ mIndex, "RTX", "enabled" };

        /// How far out from the eye the world is built, in cells. **Read every frame**, so moving
        /// it moves the world's edge without a restart.
        ///
        /// **How much world exists, which is a property of the structure rays are cast against and
        /// not of the camera.** `viewing distance` is the rasterizer's fog-and-visibility knob and
        /// means something else: at 7168 against a cell of 8192 it barely leaves the active grid.
        ///
        /// The air is tuned to this as well as the ground, because fog measured in one distance over
        /// a world built to another is a world you cannot see — see `Rtx::distantLandReach`.
        SettingValue<float> mDistantLandCells{ mIndex, "RTX", "distant land cells" };

        /// How hard DLSS Ray Reconstruction works, or `off` for none of it.
        ///
        /// A name rather than a number, and unrecognised is refused rather than defaulted — see
        /// `Rtx::upscaleNamed`. **No menu offers `off`**, for the reason `Rtx::sUpscaleModes` gives.
        ///
        /// **Changing it rebuilds every target**, which `Rtx::Renderer::setUpscale` does and the
        /// window manager asks for when this changes. A machine that cannot reach the mode keeps
        /// the one it had and says so in the log.
        SettingValue<std::string> mUpscale{ mIndex, "RTX", "upscale" };

        /// Which Ray Reconstruction network runs, where one runs at all.
        ///
        /// A name rather than a number, refused rather than defaulted when unrecognised — see
        /// `Rtx::presetNamed`. Ray Reconstruction keeps its own presets, which are not
        /// super-resolution's.
        SettingValue<std::string> mPreset{ mIndex, "RTX", "preset" };

        /// How long every frame stands for, in seconds, or nought to time each one.
        ///
        /// **What makes two runs of one build the same run.** Everything the world animates steps
        /// by what the last frame took, so a build that draws a frame in four milliseconds animates
        /// further per frame than one that takes twenty — and the two then measure different
        /// scenes and draw different pictures. A measured run states the step instead, and ten
        /// seconds of world is six hundred frames on every machine.
        ///
        /// **Nought for anybody playing**, which is the wall clock and the only thing a played
        /// session should have.
        SettingValue<float> mFixedStep{ mIndex, "RTX", "fixed step" };

        /// A run to make instead of a session to play, as `Rtx::readSpec` spells one.
        ///
        /// **What lets the game measure itself.** Where to stand is a savegame's business, so this
        /// says only how long, how much of it warms up, and how fast to fly — `600`, `10s`,
        /// `10s:2s`, `10s:2s@12000`. Empty is a session somebody is playing.
        SettingValue<std::string> mSession{ mIndex, "RTX", "session" };

        /// How much of the lighting painted into each vanilla texture to divide back out, from
        /// nought to one. Nought shows the textures as they were drawn, lighting and all, which is
        /// the A/B that says what the recovery did.
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

        /// Whether the trace also counts the see-through surfaces each primary ray crosses.
        ///
        /// **Off, because it is a second traversal a pixel.** A frame time taken under it is a
        /// measurement of the census rather than of the picture.
        SettingValue<bool> mCountCrossings{ mIndex, "RTX", "count crossings" };

        /// How the trace sorts its threads between the traversal and the shader that resolves what
        /// they found: `off`, `hit` or `hint`. Off is what the others are measured against.
        SettingValue<std::string> mReorder{ mIndex, "RTX", "reorder" };
    };
}

#endif

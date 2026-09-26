#include <gtest/gtest.h>

#include <apps/openmw/mwrender/rtx/rtxsettings.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/pacing.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/specularlayout.hpp>
#include <components/rtx/upscale.hpp>

namespace MWRender
{
    namespace
    {
        /// A set every field of which differs from its default, so a field the derivation drops
        /// or swaps with another shows.
        RtxSettingValues valid()
        {
            return RtxSettingValues{
                .mUpscale = "balanced",
                .mPreset = "e",
                .mReflex = "boost",
                .mDistantLandCells = 6.0f,
                .mViewingDistance = 7168.0f,
                .mObjectPaging = false,
                .mObjectPagingMinSize = 0.025f,
                .mSpecularMapLayout = "metal roughness",
            };
        }

        /// **Each spelling means its own mode, and the cells mean the reach**, for the game and the
        /// harness alike, since both derive through this.
        TEST(RtxSettingsTest, eachSpellingDerivesItsModeAndTheCellsTheReach)
        {
            for (const auto& [mode, spelling] : Rtx::sUpscaleNames.mNames)
            {
                RtxSettingValues values = valid();
                values.mUpscale = spelling;
                EXPECT_EQ(RtxSettings::derive(values).mUpscaling.mMode, mode) << spelling;
            }
            for (const auto& [preset, spelling] : Rtx::sPresetNames.mNames)
            {
                RtxSettingValues values = valid();
                values.mPreset = spelling;
                EXPECT_EQ(RtxSettings::derive(values).mUpscaling.mPreset, preset) << spelling;
            }
            for (const auto& [mode, spelling] : Rtx::sLatencyModeNames.mNames)
            {
                RtxSettingValues values = valid();
                values.mReflex = spelling;
                EXPECT_EQ(RtxSettings::derive(values).mLatency, mode) << spelling;
            }
            for (const auto& [layout, spelling] : Rtx::sSpecularLayoutNames.mNames)
            {
                RtxSettingValues values = valid();
                values.mSpecularMapLayout = spelling;
                EXPECT_EQ(RtxSettings::derive(values).mMirror.mSpecularLayout, layout) << spelling;
            }

            const RtxSettings derived = RtxSettings::derive(valid());
            EXPECT_EQ(derived.mUpscaling.mMode, Rtx::Upscale::Balanced);
            EXPECT_EQ(derived.mUpscaling.mPreset, Rtx::Preset::E);
            EXPECT_EQ(derived.mLatency, Rtx::LatencyMode::Boost);
            EXPECT_EQ(derived.mMirror.mReach, 49152.0f) << "six cells of 8192 units";
            EXPECT_FALSE(derived.mMirror.mDistantStatics);
            EXPECT_EQ(derived.mMirror.mMinSize, 0.025f);
            EXPECT_EQ(derived.mMirror.mSpecularLayout, Rtx::SpecularLayout::MetalRoughness);

            RtxSettingValues handedBack = valid();
            handedBack.mDistantLandCells = 0.0f;
            EXPECT_EQ(RtxSettings::derive(handedBack).mMirror.mReach, 7168.0f)
                << "nought cells hands the reach to the viewing distance";
        }

        /// A spelling that names no mode is refused rather than defaulted, whichever of the three
        /// it is: a typo that quietly traced under `off` would be a session of the wrong picture.
        TEST(RtxSettingsTest, aSpellingNoModeHasIsRefused)
        {
            RtxSettingValues upscale = valid();
            upscale.mUpscale = "Quality";
            EXPECT_THROW(RtxSettings::derive(upscale), Rtx::InputError);

            RtxSettingValues preset = valid();
            preset.mPreset = "k";
            EXPECT_THROW(RtxSettings::derive(preset), Rtx::InputError);

            RtxSettingValues reflex = valid();
            reflex.mReflex = "fast";
            EXPECT_THROW(RtxSettings::derive(reflex), Rtx::InputError);

            // The classic layout is the one a `_spec` file most often has, and it has no name here:
            // its maps are what `ignore` is for.
            RtxSettingValues layout = valid();
            layout.mSpecularMapLayout = "classic";
            EXPECT_THROW(RtxSettings::derive(layout), Rtx::InputError);
        }
    }
}

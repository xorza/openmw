#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/instancerecord.hpp>

#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/accumulate.h>
#include <components/rtx/spritelight.hpp>
#include <components/rtx/wavecascade.hpp>
#include <components/rtx/wavespectrum.hpp>
#include <components/rtxvulkan/buffer.hpp>
#include <components/rtxvulkan/commands.hpp>
#include <components/rtxvulkan/compositepass.hpp>
#include <components/rtxvulkan/fogtile.hpp>
#include <components/rtxvulkan/fogvolume.hpp>
#include <components/rtxvulkan/gbuffer.hpp>
#include <components/rtxvulkan/graveyard.hpp>
#include <components/rtxvulkan/image.hpp>
#include <components/rtxvulkan/result.hpp>
#include <components/rtxvulkan/sceneacceleration.hpp>
#include <components/rtxvulkan/scenebuffers.hpp>
#include <components/rtxvulkan/texture.hpp>
#include <components/rtxvulkan/visibilitypass.hpp>
#include <components/rtxvulkan/wavepass.hpp>

#include "../allocations.hpp"
#include "../harness.hpp"
#include "../wavemoments.hpp"

namespace Rtx::Testing
{
    /// Two triangles of a quad, wound so its face points the way its corners were listed.
    inline constexpr std::array<std::uint32_t, 6> sQuadIndices{ 0, 1, 2, 0, 2, 3 };

    /// The unit square, in the same corner order — a texture laid once across a quad.
    inline const std::array<osg::Vec2f, 4> sQuadUv{
        osg::Vec2f(0.0f, 0.0f),
        osg::Vec2f(1.0f, 0.0f),
        osg::Vec2f(1.0f, 1.0f),
        osg::Vec2f(0.0f, 1.0f),
    };

    /// A level square of `extent` about the origin at height `z`, facing up.
    inline std::array<osg::Vec3f, 4> makeSheet(float extent, float z)
    {
        return {
            osg::Vec3f(-extent, -extent, z),
            osg::Vec3f(extent, -extent, z),
            osg::Vec3f(extent, extent, z),
            osg::Vec3f(-extent, extent, z),
        };
    }

    /// A level sheet of water `extent` across at z = 0, with nothing under it.
    inline SceneDesc makeOpenWater(float extent)
    {

        SceneDesc scene;
        Material water;
        water.mKind = MaterialKind::Water;
        scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
            .mMesh = scene.addMesh(makeSheet(extent, 0.0f), {}, {}, sQuadIndices),
            .mMaterial = scene.addMaterial(water) });

        return scene;
    }

    /// A level bed `depth` units under a level surface of water, both `extent` across.
    ///
    /// The shape most of the water tests want: something to see through the water at, and the
    /// water to see it through.
    inline SceneDesc makeFlooded(float extent, float depth)
    {

        SceneDesc scene = makeOpenWater(extent);
        scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
            .mMesh = scene.addMesh(makeSheet(extent, -depth), {}, {}, sQuadIndices) });

        return scene;
    }

    /// How bright the sun is in the tests that measure through water.
    ///
    /// Named because their arithmetic uses it as well: the number the shader is handed and the
    /// number an expectation is computed from have to be one number, or the test quietly stops
    /// describing the shader.
    inline constexpr float sSunOverWater = 2.0f;

    /// Where the sun stands, from how far it is off the vertical. Its light travels back along
    /// this, which is the only sun vector the shader has.
    inline osg::Vec3f sunStandingAt(float zenith)
    {
        return osg::Vec3f(0.0f, -std::sin(zenith), std::cos(zenith));
    }

    /// Where the sun stands over the tests that measure through water — and it must not be
    /// straight up.
    ///
    /// Overhead, the sun's own mirror image lands exactly where a camera looking straight down
    /// sends its reflection ray, and a saturated disc of sun in the middle of the frame is not
    /// what these are measuring. Two degrees puts it twelve disc-widths clear of the reflection
    /// and costs the arithmetic a part in a thousand, which `throughFlatWater` accounts for
    /// anyway.
    inline const float sNearlyOverhead = osg::DegreesToRadians(2.0f);

    /// Puts `camera` over a flooded scene with nothing in its sky but the sun.
    ///
    /// The water level is the one `makeFlooded` builds to, and the sky is black — so apart from
    /// the sun's own disc, the two per cent that reflects off the surface reflects nothing.
    /// Every byte in the frame has then come up through the depth, which is what lets these
    /// tests name an exact value.
    inline void litThroughWater(Shaders::VisibilityConstants& camera, float zenith = sNearlyOverhead)
    {
        camera.mSunPosition = sunStandingAt(zenith);
        camera.mSunIrradiance = osg::Vec3f(sSunOverWater, sSunOverWater, sSunOverWater);
        camera.mSkyHorizon = osg::Vec3f();
        camera.mSkyZenith = osg::Vec3f();
        camera.mWaterLevel = 0.0f;
    }

    /// A square in the xz plane at y = 0, facing along -Y, four hundred units across, which is
    /// larger than any frame at the distances most of these tests use.
    inline const std::array<osg::Vec3f, 4> sWallQuad{
        osg::Vec3f(-200.0f, 0.0f, -200.0f),
        osg::Vec3f(200.0f, 0.0f, -200.0f),
        osg::Vec3f(200.0f, 0.0f, 200.0f),
        osg::Vec3f(-200.0f, 0.0f, 200.0f),
    };

    /// That wall, on its own, as a scene.
    ///
    /// @param scale what to stretch it by, for a frame taken far enough away that four hundred
    ///        units is a fraction of one pixel.
    inline SceneDesc makeWall(float scale = 1.0f)
    {
        SceneDesc scene;
        const Index mesh = scene.addMesh(sWallQuad, {}, {}, sQuadIndices);
        scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::scale(scale, 1.0f, scale), .mMesh = mesh });
        return scene;
    }

    /// A linear value as the display curve writes it, so a test can name the byte it expects.
    ///
    /// **The display curve and not the whole of what `tone.comp` does**: that pass runs
    /// `toneMap` first, and a test measuring what the trace computed wants the radiance rather
    /// than the picture made of it. `countHits` encodes with this for the same reason.
    inline std::uint8_t encodeSrgb(float linear)
    {
        const float encoded = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        return static_cast<std::uint8_t>(std::lround(std::clamp(encoded, 0.0f, 1.0f) * 255.0f));
    }

    /// A byte the shader wrote, back to the linear value behind it.
    ///
    /// Ratios have to be taken in linear. sRGB is a power curve, so the same proportional
    /// brightening is a different number of bytes at the top of the range and at the bottom.
    inline float decodeSrgb(std::uint8_t byte)
    {
        const float encoded = static_cast<float>(byte) / 255.0f;
        return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    }

    /// How brightly the sky is lit, in the tests that measure a wall through fog or against the
    /// world's edge.
    ///
    /// Named for the reason `sSunOverWater` is: each test's arithmetic uses it as well as handing
    /// it to the shader, and the two have to be one number.
    ///
    /// **The sky rather than the cell's ambient, because a wall is lit by what it can see.** The
    /// ambient is what terminates a path now, one bounce further along; what fills a surface the
    /// eye is looking at is the hemisphere it gathers. A sky of one radiance makes that gather
    /// exact rather than noisy — every direction returns the same number, so one sample is the
    /// whole answer.
    inline constexpr float sFoggySky = 0.6f;

    /// A texture a test builds by hand, and the storage its description spans.
    ///
    /// Filled in place rather than returned, because `TextureData` carries spans into these
    /// vectors and nothing should have to reason about whether a move kept their buffers.
    struct TestTexture
    {
        std::vector<std::uint8_t> mBytes;
        std::vector<MipLevel> mLevels;
        TextureData mData;
    };

    /// A texture whose every mip is one flat colour — level `i` is `40 + 30i`, evenly spaced
    /// and none of them black.
    ///
    /// **The byte a ray comes back with reads out the level it sampled**, and because the levels
    /// are evenly spaced in value, `textureLod` blending two of them lands exactly on
    /// `40 + 30 * lod`. A *fractional* level is readable that way, which is what makes a cone's
    /// width measurable rather than merely orderable. Flat colours also mean the answer does not
    /// depend on where in the texture the cone landed.
    inline void makeMipLadder(TestTexture& texture)
    {
        constexpr std::uint32_t extent = 64;
        constexpr std::uint32_t levels = 7;

        for (std::uint32_t level = 0; level < levels; ++level)
        {
            const std::uint32_t side = extent >> level;
            texture.mLevels.push_back(MipLevel{ static_cast<std::uint32_t>(texture.mBytes.size()), side, side });
            texture.mBytes.insert(
                texture.mBytes.end(), std::size_t{ side } * side * 4, static_cast<std::uint8_t>(40 + 30 * level));
        }

        texture.mData = TextureData{
            .mFormat = TextureFormat::Rgba8Unorm,
            .mWidth = extent,
            .mHeight = extent,
            .mBytes = std::as_bytes(std::span(texture.mBytes)),
            .mLevels = texture.mLevels,
            .mName = "mip ladder",
        };
    }

    /// A texture that is white and wholly opaque, at one level.
    ///
    /// **What `makeMipLadder` cannot be.** Its levels encode which one was sampled, so its alpha
    /// is the level's own byte and its first is 40 of 255 — a sprite cut from it covers a sixth
    /// of what is behind it, which is a fine thing to be seen through and no use at all for
    /// asking what happens when a sprite owns a pixel.
    inline void makeOpaqueSheet(TestTexture& texture)
    {
        constexpr std::uint32_t extent = 4;

        texture.mLevels.push_back(MipLevel{ 0, extent, extent });
        texture.mBytes.assign(std::size_t{ extent } * extent * 4, std::uint8_t{ 255 });

        texture.mData = TextureData{
            .mFormat = TextureFormat::Rgba8Unorm,
            .mWidth = extent,
            .mHeight = extent,
            .mBytes = std::as_bytes(std::span(texture.mBytes)),
            .mLevels = texture.mLevels,
            .mName = "opaque sheet",
        };
    }

    class RtxVisibilityTest : public Testing::RendererTest
    {
    protected:
        /// The luminance of every pixel of a frame that holds nothing but air, from a camera
        /// standing in white fog of one thickness.
        ///
        /// **A wall behind the camera, because a scene has to hold something.** Every ray runs
        /// to `FOG_REACH` and comes back with air and the sky, which is what makes the frame a
        /// measurement of the air alone — and what lets two tests ask about the field's amount
        /// and the field's motion off one fixture.
        ///
        /// @param camera has its fog colour and thickness set here; whatever else a test set on
        ///        it — the coverage, the wind, the moment — stays.
        void airThrough(Shaders::VisibilityConstants camera, std::uint32_t size, std::vector<float>& luminance)
        {
            camera.mFogColour = osg::Vec3f(1.0f, 1.0f, 1.0f);
            camera.mFogExtinction = 3.0e-6f;

            const SceneDesc scene = makeWall();
            std::vector<std::uint8_t> pixels;
            countHits(scene, {}, camera, size, pixels);

            luminance.resize(std::size_t{ size } * size);
            for (std::size_t i = 0; i < luminance.size(); ++i)
                luminance[i] = decodeSrgb(pixels[i * 4]);
        }

        /// Renders `scene` at `size` square and returns how many primary rays hit.
        /// @param sea what the water is doing. A state with no height in it is a flat sea, which
        ///        is what a test asserting an exact transmittance through one needs.
        /// @param accumulate how many differently-seeded frames to average, overriding the
        ///        camera's own frame index with 0, 1, ... Zero renders the camera's frame alone.
        /// @param filter whether the denoiser runs, and off by default on purpose. Almost
        ///        every test over this fixture asserts a radiance a particular pixel must have, and a
        ///        filter mixes its neighbours into it — a test that let one run would be measuring the
        ///        denoiser rather than the thing it was written to measure. The tests that are
        ///        about the filter ask for it.
        std::uint32_t countHits(const SceneDesc& scene, std::span<const TextureData> textures,
            const Shaders::VisibilityConstants& camera, std::uint32_t size, std::vector<std::uint8_t>& pixels,
            const SeaState& sea = SeaState{}, std::uint32_t accumulate = 0, bool filter = false, bool jitter = false,
            std::optional<float> exposure = 1.0f)
        {
            mRenderer->resize(size, size);
            mRenderer->setScene(Rtx::sWorld, scene, inSceneOrder(textures), sea);

            // One frame per sample, where this used to record several dispatches into a single
            // submit. Each is waited out before the next, which orders them, and the renderer's
            // own history barrier is what makes each sum visible to the next.
            const std::uint32_t frames = std::max(accumulate, 1u);
            std::uint32_t hits = 0;
            for (std::uint32_t frame = 0; frame < frames; ++frame)
            {
                Shaders::VisibilityConstants sampled = camera;
                if (accumulate > 0)
                    sampled.mFrame = frame;

                mRenderer->renderFrame(sampled,
                    FrameOptions{ .mAccumulate = accumulate > 0 ? frame + 1 : 0,
                        .mJitter = jitter,
                        .mFilter = filter,
                        .mExposure = exposure });

                // Every frame hits the same primary geometry, so the last one's count is the
                // answer rather than a sum to be divided back down.
                hits = mRenderer->finishFrame().value().mHits;
            }

            // **The radiance encoded here rather than the picture read back.** `tone.comp` puts
            // `toneMap` between the two, and that curve is a display transform: it takes 0.04
            // off a shadow and rolls a highlight away from one, neither of which a test about
            // what the trace computed has an opinion on. Every figure over this fixture was
            // derived against the radiance through the display curve, which is this.
            //
            // `mExposure` is one for every caller of this, so there is no measured exposure to
            // fold in — see the `std::optional` default above.
            mRenderer->readChannel(Channel::Radiance, mRadiance);

            pixels.resize(mRadiance.size());
            for (std::size_t at = 0; at < mRadiance.size(); at += 4)
            {
                for (std::size_t channel = 0; channel < 3; ++channel)
                    pixels[at + channel] = encodeSrgb(mRadiance[at + channel]);

                // Coverage rather than radiance, which the curve does not touch either.
                pixels[at + 3]
                    = static_cast<std::uint8_t>(std::lround(std::clamp(mRadiance[at + 3], 0.0f, 1.0f) * 255.0f));
            }

            return hits;
        }

        /// The mean of one channel of the last render, in linear radiance.
        ///
        /// **The frame and not a pixel, where every pixel of it is the same measurement.** The
        /// estimator is one sample per pixel, so a frame lit evenly is as many samples as it has
        /// pixels and its mean is the figure with that error divided down — which is what lets a
        /// test hold a derived number to three decimal places over a stochastic renderer.
        float meanRadiance(std::size_t channel = 0) const
        {
            float sum = 0.0f;
            for (std::size_t at = channel; at < mRadiance.size(); at += 4)
                sum += mRadiance[at];

            return sum / float(mRadiance.size() / 4);
        }

        /// The picture a display would show: this pass's own tone curve and display curve over
        /// the exposure the frame measured for itself.
        ///
        /// **The one thing here that wants the picture rather than the radiance**, because what
        /// it measures is the exposure pass. Every other test over this fixture is about what the
        /// trace computed, which `countHits` gives without a display transform over it.
        void renderPicture(const SceneDesc& scene, const Shaders::VisibilityConstants& camera, std::uint32_t size,
            std::vector<std::uint8_t>& pixels, std::span<const TextureData> textures = {})
        {
            mRenderer->resize(size, size);
            mRenderer->setScene(Rtx::sWorld, scene, inSceneOrder(textures), SeaState{});
            mRenderer->renderFrame(camera, FrameOptions{ .mExposure = std::nullopt });
            mRenderer->readPixels(pixels);
        }

        /// The same render as `countHits`, read back in linear radiance rather than as bytes.
        ///
        /// **What a figure is measured on.** `readPixels` gives the picture a display would
        /// show — eight bits, after the tone curve and the display curve — and the filter
        /// figures had reached the point where that was the quantiser talking: two thirds
        /// of a byte at the brightness they sit at. This is the same frame before either.
        void renderRadiance(const SceneDesc& scene, const Shaders::VisibilityConstants& camera, std::uint32_t size,
            std::vector<float>& values, std::uint32_t accumulate = 0, bool filter = false)
        {
            // The bytes are made and thrown away: they are what sharing `countHits`'s render
            // loop costs, and encoding one image against a sixty-four frame render is not a
            // trade worth a second copy of that loop.
            std::vector<std::uint8_t> pixels;
            countHits(scene, {}, camera, size, pixels, SeaState{}, accumulate, filter);
            mRenderer->readChannel(Channel::Radiance, values);
        }

        /// A run of filtered frames with the history let build, read back in linear radiance.
        ///
        /// **Not `countHits` with a count on it, and the difference is the whole point.**
        /// `mAccumulate` averages finished frames in the composite; this leaves that at nought
        /// and advances `mFrame` instead, so each frame is a different draw and the only thing
        /// combining them is the accumulator under test. `resetHistory` first, because a
        /// one-frame baseline taken after a longer run is a baseline that already has a history
        /// in it — which reads as the accumulator doing nothing at all.
        ///
        /// @param first the sampler's frame the run starts at, so a run can be handed a stream of its
        ///        own rather than the one every other run in the test consumed.
        void renderFiltered(const SceneDesc& scene, const Shaders::VisibilityConstants& camera, std::uint32_t size,
            std::vector<float>& values, std::uint32_t frames, std::uint32_t first = 0)
        {
            mRenderer->resize(size, size);
            mRenderer->setScene(Rtx::sWorld, scene, {}, SeaState{});
            mRenderer->resetHistory();

            for (std::uint32_t frame = 0; frame < frames; ++frame)
            {
                Shaders::VisibilityConstants sampled = camera;
                sampled.mFrame = first + frame;
                mRenderer->renderFrame(sampled, FrameOptions{ .mAccumulate = 0, .mFilter = true, .mExposure = 1.0f });
            }

            mRenderer->readChannel(Channel::Radiance, values);
        }

        /// What one pixel read on each frame of a run, with the history let build across them.
        ///
        /// **The frames apart rather than averaged, which is what a test about flicker needs.**
        /// `countHits` with an `accumulate` on it hands back the mean of a run and says nothing
        /// about how far the frames stood from each other — and how far they stand is the whole
        /// of what a boiling image is. So this advances `mFrame` and reads the pixel out after
        /// every frame, leaving the composite's accumulator and the denoiser off, so what moves
        /// between two entries moved in the trace.
        void radianceFrameByFrame(const SceneDesc& scene, const Shaders::VisibilityConstants& camera,
            std::uint32_t size, std::uint32_t frames, std::size_t pixel, std::vector<float>& radiance)
        {
            mRenderer->resize(size, size);
            mRenderer->setScene(Rtx::sWorld, scene, {}, SeaState{});
            mRenderer->resetHistory();

            radiance.clear();
            radiance.reserve(frames);

            std::vector<float> values;
            for (std::uint32_t frame = 0; frame < frames; ++frame)
            {
                Shaders::VisibilityConstants sampled = camera;
                sampled.mFrame = frame;

                mRenderer->renderFrame(sampled, FrameOptions{ .mAccumulate = 0, .mFilter = false, .mExposure = 1.0f });
                mRenderer->finishFrame();
                mRenderer->readChannel(Channel::Radiance, values);

                radiance.push_back(values[pixel * 4]);
            }
        }

        /// A wall square to the sun with one pane held in front of it, as the byte its centre
        /// pixel comes back as.
        ///
        /// **Shared by the three tests that hold a pane up to different rays.** One asks what
        /// the pane let past to a shadow ray, one asks what the eye saw through it, and one
        /// fades the placement rather than the material. The evidence in all three is that the
        /// answer lands where a sun of half the irradiance lands. That is evidence only while
        /// none of them can drift from the others' scene.
        ///
        /// @param pane where to put the quad — on the sun's path to the wall for a shadow, on
        ///        the eye's path for a peel.
        /// @param colour what the pane is made of, its own alpha included, or nothing at all for
        ///        the wall on its own.
        /// @param fade how much of the pane the game is showing, which is the other of the two
        ///        numbers a surface's opacity is made of.
        std::uint8_t litThroughPane(std::span<const osg::Vec3f> pane, std::optional<osg::Vec4f> colour,
            const osg::Vec3f& irradiance, float fade = 1.0f)
        {
            constexpr std::uint32_t size = 33;

            SceneDesc scene = makeWall();
            if (colour.has_value())
            {
                const Index glass = scene.addMaterial(Material{
                    .mDiffuseColour = *colour,
                    .mAlphaMode = AlphaMode::Blend,
                });

                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(pane, {}, {}, sQuadIndices),
                    .mMaterial = glass,
                    .mOpacity = fade });
            }

            Shaders::VisibilityConstants camera = makeCamera(
                osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mSunPosition = osg::Vec3f(0.0f, -1.0f, 0.0f);
            camera.mSunIrradiance = irradiance;

            std::vector<std::uint8_t> pixels;
            EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);

            return pixels[(std::size_t{ size / 2 } * size + size / 2) * 4];
        }

        /// A wall square to the sun with a pane held twenty units across at y = -50, as the three
        /// bytes its centre pixel comes back as.
        ///
        /// **What the water test and the first-person test share**, each placing the pane its
        /// own way through `place`. The camera stands off the sun's axis, so the centre pixel
        /// lands on the patch of wall the pane shadows without the camera's own ray having to
        /// cross the pane — it passes y = -50 at x = 50, and the pane reaches 20 — or straight
        /// at the pane, to see it at all.
        std::array<std::uint8_t, 3> paneOverWall(
            const std::function<void(SceneDesc&, std::span<const osg::Vec3f>)>& place, bool lookAtIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = (std::size_t{ size / 2 } * size + size / 2) * 4;

            const std::array pane{
                osg::Vec3f(-20.0f, -50.0f, -20.0f),
                osg::Vec3f(20.0f, -50.0f, -20.0f),
                osg::Vec3f(20.0f, -50.0f, 20.0f),
                osg::Vec3f(-20.0f, -50.0f, 20.0f),
            };

            SceneDesc scene = makeWall();
            place(scene, pane);

            Shaders::VisibilityConstants camera = lookAtIt
                ? makeCamera(
                    osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, -50.0f, 0.0f), 60.0f, size, size, 10000.0f)
                : makeCamera(
                    osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mSunPosition = osg::Vec3f(0.0f, -1.0f, 0.0f);
            camera.mSunIrradiance = osg::Vec3f(2.0f, 2.0f, 2.0f);

            std::vector<std::uint8_t> pixels;
            EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);
            return { pixels[centre], pixels[centre + 1], pixels[centre + 2] };
        }

        /// The fixture's textures, numbered the way its scene added them.
        ///
        /// **A convention of these tests and not of the renderer.** Every test here builds its
        /// descriptions in the order its scene calls `addTexture`, so position is slot. The
        /// array used to assume that of every caller, which is a trap for the one whose scene
        /// has given a slot back: its table has a hole in it and its descriptions do not.
        std::span<const TextureData> inSceneOrder(std::span<const TextureData> textures)
        {
            mNumbered.assign(textures.begin(), textures.end());
            for (std::size_t at = 0; at < mNumbered.size(); ++at)
                mNumbered[at].mSlot = static_cast<std::uint32_t>(at);

            return mNumbered;
        }

        std::vector<float> mRadiance;
        std::vector<TextureData> mNumbered;
    };
}

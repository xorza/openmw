#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <source_location>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/camera.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/frameworld.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/renderer.hpp>
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
#include <components/rtxvulkan/skinpass.hpp>
#include <components/rtxvulkan/skintables.hpp>
#include <components/rtxvulkan/texture.hpp>
#include <components/rtxvulkan/visibilitypass.hpp>
#include <components/rtxvulkan/wavepass.hpp>

#include "../allocations.hpp"
#include "../geometry.hpp"
#include "../harness.hpp"
#include "../testtexture.hpp"
#include "../wavemoments.hpp"

namespace Rtx::Testing
{
    /// A level sheet of water `extent` across at z = 0, with nothing under it.
    inline SceneDesc makeOpenWater(float extent)
    {
        SceneDesc scene;
        Material water;
        water.mKind = MaterialKind::Water;
        scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
            .mMesh = scene.addMesh(sheetAt(extent, 0.0f), {}, {}, sQuadIndices),
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
            .mMesh = scene.addMesh(sheetAt(extent, -depth), {}, {}, sQuadIndices) });

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

    /// The wall `sWallQuad` names, on its own, as a scene.
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

    /// One see-through quad in `scene`, of `colour` and its own alpha, faded as the game fades a
    /// placement.
    ///
    /// **The two numbers an opacity is made of, added the one way.** The shader multiplies a
    /// material's alpha by a placement's fade, and a helper that built either of them its own way
    /// would be holding up a surface this renderer does not have.
    inline void addPane(SceneDesc& scene, std::span<const osg::Vec3f> quad, const osg::Vec4f& colour, float fade = 1.0f)
    {
        const Index glass = scene.addMaterial(Material{
            .mDiffuseColour = colour,
            .mAlphaMode = AlphaMode::Blend,
        });

        scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
            .mMesh = scene.addMesh(quad, {}, {}, sQuadIndices),
            .mMaterial = glass,
            .mOpacity = fade });
    }

    /// The eye and the sun every test over that wall stands it under: the sun along +Y, square to
    /// the wall, and the eye off that axis at x = 100 looking at the middle of it.
    ///
    /// **One camera, because each of these tests is read as a ratio against the wall's own byte.**
    /// The figures they are pinned to hold only while the wall, the eye and the sun are the same in
    /// every one of them — and off the sun's axis is what lets a pane on the eye's ray leave the
    /// patch of wall the centre pixel looks at fully lit.
    ///
    /// @param origin,target where to aim it instead, for the one test that has to see the pane
    ///        rather than what it shadows.
    inline Shaders::VisibilityConstants wallCamera(std::uint32_t size, const osg::Vec3f& irradiance,
        const osg::Vec3f& origin = osg::Vec3f(100.0f, -100.0f, 0.0f), const osg::Vec3f& target = osg::Vec3f())
    {
        Shaders::VisibilityConstants camera = makeCamera(origin, target, 60.0f, size, size, 10000.0f);
        camera.mSunPosition = osg::Vec3f(0.0f, -1.0f, 0.0f);
        camera.mSunIrradiance = irradiance;

        return camera;
    }

    /// Which pixel of a `size` by `size` frame is its middle one — `size / 2` along each axis,
    /// which is the pixel just past the centre where `size` is even.
    inline constexpr std::size_t centreOf(std::uint32_t size)
    {
        return std::size_t{ size / 2 } * size + size / 2;
    }

    /// The first of the four values that pixel holds, which is how a read-back over this fixture is
    /// indexed: a byte frame and a radiance frame both carry four values a pixel.
    inline constexpr std::size_t centreValueOf(std::uint32_t size)
    {
        return centreOf(size) * 4;
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

    /// A texture whose every mip is one flat colour — level `i` is `40 + 30i`, evenly spaced
    /// and none of them black.
    ///
    /// **The byte a ray comes back with reads out the level it sampled**, and because the levels
    /// are evenly spaced in value, `textureLod` blending two of them lands exactly on
    /// `40 + 30 * lod`. A *fractional* level is readable that way, which is what makes a cone's
    /// width measurable rather than merely orderable. Flat colours also mean the answer does not
    /// depend on where in the texture the cone landed.
    inline void paintMipLadder(TestTexture& texture)
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

        texture.describe(extent, extent, "mip ladder");
    }

    /// A texture that is white and wholly opaque, at one level.
    ///
    /// **What `paintMipLadder` cannot be.** Its levels encode which one was sampled, so its alpha
    /// is the level's own byte and its first is 40 of 255 — a sprite cut from it covers a sixth
    /// of what is behind it, which is a fine thing to be seen through and no use at all for
    /// asking what happens when a sprite owns a pixel.
    inline void paintOpaqueSheet(TestTexture& texture)
    {
        constexpr std::uint32_t extent = 4;

        texture.mLevels.push_back(MipLevel{ 0, extent, extent });
        texture.mBytes.assign(std::size_t{ extent } * extent * 4, std::uint8_t{ 255 });

        texture.describe(extent, extent, "opaque sheet");
    }

    /// Everything a render over this fixture decides beyond the scene, the camera and the extent.
    ///
    /// **Named rather than positional, because a tail of defaulted arguments says nothing.** Five
    /// of these read at a call site as `SeaState{}, 16, false, true`, which no reader can tell from
    /// `SeaState{}, 16, true, false` — and the two are a jittered run and a filtered one.
    struct Shot
    {
        /// What the water is doing. A state with no height in it is a flat sea, which is what a
        /// test asserting an exact transmittance through one needs.
        SeaState mSea;

        /// How many frames the run draws, each at its own sampler index counting from
        /// `mFirstFrame`, so the frames are different draws rather than one draw repeated.
        ///
        /// Zero draws one frame at the camera's own index and leaves it there, which is what a test
        /// that sets `mFrame` for itself needs.
        std::uint32_t mFrames = 0;

        /// Whether the composite averages the run into one picture, rather than leaving each frame
        /// to stand on its own.
        ///
        /// **Off is what a test of the accumulator wants.** Averaged, a run arrives as its mean and
        /// nothing is left of how far the frames stood from each other; unaveraged, the only thing
        /// combining them is the accumulator under test. Nothing either way where `mFrames` is
        /// zero, since one frame is not a run.
        bool mAverage = true;

        /// The sampler's frame the run starts at, so a run can be handed a stream of its own rather
        /// than the one every other run in the test consumed.
        std::uint32_t mFirstFrame = 0;

        /// Whether the denoiser runs, and off by default on purpose. Almost every test over this
        /// fixture asserts a radiance a particular pixel must have, and a filter mixes its
        /// neighbours into it — a test that let one run would be measuring the denoiser rather than
        /// the thing it was written to measure. The tests that are about the filter ask for it.
        bool mFilter = false;

        /// Whether the sample point moves inside its pixel, which buys nothing on a single frame
        /// and is what several of them cover between them.
        bool mJitter = false;

        /// Throws the denoiser's history away before the run.
        ///
        /// **A one-frame baseline taken after a longer run is a baseline that already has a history
        /// in it**, which reads as the accumulator doing nothing at all.
        bool mResetHistory = false;

        /// The exposure the composite is held at. `std::nullopt` is the exposure the frame measures
        /// for itself, which is what a test about the exposure pass wants — and what every figure
        /// derived through `countHits` must not have, since those are about what the trace computed.
        std::optional<float> mExposure = 1.0f;
    };

    class RtxVisibilityTest : public Testing::RendererTest
    {
    protected:
        /// Draws `scene` at `size` square, and returns how many primary rays hit.
        ///
        /// **The one render loop over this fixture.** Every helper below reaches the device through
        /// here, so what a shot means is said once rather than once per way of reading the answer.
        ///
        /// The frame is left on the device, where `readRadiance` and `readPixels` can ask for it.
        ///
        /// @param afterEach run once each frame is finished, for a caller measuring what moves
        ///        between two frames rather than what a run of them averages to.
        std::uint32_t renderShot(const SceneDesc& scene, std::span<const TextureData> textures,
            const Shaders::VisibilityConstants& camera, std::uint32_t size, const Shot& shot = {},
            const std::function<void()>& afterEach = {})
        {
            mRenderer->resize(size, size);
            mRenderer->setScene(Rtx::sWorld, scene, inSceneOrder(textures), shot.mSea);

            if (shot.mResetHistory)
                mRenderer->resetHistory();

            // One frame per sample, each waited out before the next, which orders them — and the
            // renderer's own history barrier is what makes each sum visible to the next.
            const std::uint32_t drawn = std::max(shot.mFrames, 1u);
            std::uint32_t hits = 0;
            for (std::uint32_t frame = 0; frame < drawn; ++frame)
            {
                Shaders::VisibilityConstants sampled = camera;
                if (shot.mFrames > 0)
                    sampled.mFrame = shot.mFirstFrame + frame;

                mRenderer->renderFrame(sampled,
                    FrameOptions{ .mAccumulate = shot.mFrames > 0 && shot.mAverage ? frame + 1 : 0,
                        .mJitter = shot.mJitter,
                        .mFilter = shot.mFilter,
                        .mExposure = shot.mExposure });

                // Every frame hits the same primary geometry, so the last one's count is the answer
                // rather than a sum to be divided back down.
                const std::optional<FrameResult> finished = mRenderer->finishFrame();
                if (!finished.has_value())
                    throw Error("the renderer drew a frame and gave none back");

                hits = finished->mHits;

                if (afterEach)
                    afterEach();
            }

            return hits;
        }

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

        /// Renders `scene` at `size` square and encodes what the trace computed, returning how many
        /// primary rays hit.
        ///
        /// **The radiance encoded here rather than the picture read back.** `tone.comp` puts
        /// `toneMap` between the two, and that curve is a display transform: it takes 0.04 off a
        /// shadow and rolls a highlight away from one, neither of which a test about what the trace
        /// computed has an opinion on. Every figure over this fixture was derived against the
        /// radiance through the display curve, which is this.
        std::uint32_t countHits(const SceneDesc& scene, std::span<const TextureData> textures,
            const Shaders::VisibilityConstants& camera, std::uint32_t size, std::vector<std::uint8_t>& pixels,
            const Shot& shot = {})
        {
            const std::uint32_t hits = renderShot(scene, textures, camera, size, shot);

            readRadiance(size, mRadiance);
            encodeRadiance(pixels);

            return hits;
        }

        /// What a probe read back, against the frame it asked for.
        ///
        /// **A throw and not an assertion of either kind.** Every caller indexes the buffer by a
        /// number it worked out from `size`, so a short read-back has to stop the test rather than
        /// mark it: `<cassert>` is compiled out of the build a figure is taken in, and an
        /// `ASSERT_EQ` here would return from this function and leave the caller indexing past the
        /// end of what it was handed. The message names both sizes, which is what a located failure
        /// would have had to say anyway.
        template <class T>
        static void requireFrame(const std::vector<T>& read, std::uint32_t size)
        {
            const std::size_t wanted = std::size_t{ size } * size * 4;
            if (read.size() != wanted)
                throw Error("a probe read back " + std::to_string(read.size()) + " values where a "
                    + std::to_string(size) + " by " + std::to_string(size) + " frame is " + std::to_string(wanted));
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
        void renderPicture(const SceneDesc& scene, std::span<const TextureData> textures,
            const Shaders::VisibilityConstants& camera, std::uint32_t size, std::vector<std::uint8_t>& pixels)
        {
            renderShot(scene, textures, camera, size, Shot{ .mExposure = std::nullopt });
            mRenderer->readPixels(pixels);

            requireFrame(pixels, size);
        }

        /// The same render as `countHits`, read back in linear radiance rather than as bytes.
        ///
        /// **What a figure is measured on.** `readPixels` gives the picture a display would
        /// show — eight bits, after the tone curve and the display curve — and the filter
        /// figures had reached the point where that was the quantiser talking: two thirds
        /// of a byte at the brightness they sit at. This is the same frame before either.
        void renderRadiance(const SceneDesc& scene, const Shaders::VisibilityConstants& camera, std::uint32_t size,
            std::vector<float>& values, const Shot& shot = {})
        {
            renderShot(scene, {}, camera, size, shot);
            readRadiance(size, values);
        }

        /// A run of filtered frames with the history let build, read back in linear radiance.
        ///
        /// @param first the sampler's frame the run starts at, so a run can be handed a stream of its
        ///        own rather than the one every other run in the test consumed.
        void renderFiltered(const SceneDesc& scene, const Shaders::VisibilityConstants& camera, std::uint32_t size,
            std::vector<float>& values, std::uint32_t frames, std::uint32_t first = 0)
        {
            renderRadiance(scene, camera, size, values,
                Shot{ .mFrames = frames,
                    .mAverage = false,
                    .mFirstFrame = first,
                    .mFilter = true,
                    .mResetHistory = true });
        }

        /// What one pixel read on each frame of a run, with the history let build across them.
        ///
        /// **The frames apart rather than averaged, which is what a test about flicker needs.** How
        /// far two frames stand from each other is the whole of what a boiling image is, and a mean
        /// over them says nothing about it. So this reads the pixel out after every frame, with the
        /// composite's accumulator and the denoiser both off, so what moves between two entries
        /// moved in the trace.
        void radianceFrameByFrame(const SceneDesc& scene, const Shaders::VisibilityConstants& camera,
            std::uint32_t size, std::uint32_t frames, std::size_t pixel, std::vector<float>& radiance)
        {
            radiance.clear();
            radiance.reserve(frames);

            std::vector<float> values;
            renderShot(
                scene, {}, camera, size, Shot{ .mFrames = frames, .mAverage = false, .mResetHistory = true }, [&] {
                    readRadiance(size, values);
                    radiance.push_back(values[pixel * 4]);
                });
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
        /// @param where the caller's own line, never passed. **One test holds five panes up, and
        ///        every failure would otherwise report at this helper's own line**, which says which
        ///        helper broke and not which pane.
        std::uint8_t litThroughPane(std::span<const osg::Vec3f> pane, std::optional<osg::Vec4f> colour,
            const osg::Vec3f& irradiance, float fade = 1.0f,
            std::source_location where = std::source_location::current())
        {
            const ::testing::ScopedTrace trace(where.file_name(), static_cast<int>(where.line()), "litThroughPane");

            constexpr std::uint32_t size = 33;

            SceneDesc scene = makeWall();
            if (colour.has_value())
                addPane(scene, pane, *colour, fade);

            const Shaders::VisibilityConstants camera = wallCamera(size, irradiance);

            std::vector<std::uint8_t> pixels;
            EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);

            return pixels[centreValueOf(size)];
        }

        /// A wall square to the sun with a stack of panes strung along the eye's own ray, as the
        /// byte its centre pixel comes back as.
        ///
        /// **The wall, the camera and the sun `litThroughPane` holds one pane up to**, so a stack of
        /// one is that helper's own answer and the figures the tests around it are pinned to carry
        /// over to this.
        ///
        /// The eye stands at x = 100 and looks at the origin, so its ray runs at x = -y the whole
        /// way: each pane is centred there, ten units to a side, and so is clear of the sun's own
        /// path to the middle of the wall — which travels along +Y at x = 0. The panes stand ten
        /// units apart, nearest to the eye first.
        ///
        /// @param layers what each pane is made of, its own alpha included, from the eye inwards.
        /// @param where the caller's own line, never passed, for the reason `litThroughPane` gives.
        std::uint8_t litThroughStack(std::span<const osg::Vec4f> layers, const osg::Vec3f& irradiance,
            std::source_location where = std::source_location::current())
        {
            const ::testing::ScopedTrace trace(where.file_name(), static_cast<int>(where.line()), "litThroughStack");

            constexpr std::uint32_t size = 33;

            SceneDesc scene = makeWall();
            for (std::size_t at = 0; at < layers.size(); ++at)
            {
                const float away = -60.0f + 10.0f * static_cast<float>(at);

                std::array<osg::Vec3f, 4> pane = uprightQuadAt(10.0f, away);
                for (osg::Vec3f& corner : pane)
                    corner.x() -= away;

                addPane(scene, pane, layers[at]);
            }

            const Shaders::VisibilityConstants camera = wallCamera(size, irradiance);

            std::vector<std::uint8_t> pixels;
            EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);

            return pixels[centreValueOf(size)];
        }

        /// A wall square to the sun with a pane held twenty units across at y = -50, as the three
        /// bytes its centre pixel comes back as.
        ///
        /// **What the water test and the first-person test share**, each placing the pane its
        /// own way through `place`. The camera stands off the sun's axis, so the centre pixel
        /// lands on the patch of wall the pane shadows without the camera's own ray having to
        /// cross the pane — it passes y = -50 at x = 50, and the pane reaches 20 — or straight
        /// at the pane, to see it at all.
        /// @param where the caller's own line, never passed, for the reason `litThroughPane` gives.
        std::array<std::uint8_t, 3> paneOverWall(
            const std::function<void(SceneDesc&, std::span<const osg::Vec3f>)>& place, bool lookAtIt,
            std::source_location where = std::source_location::current())
        {
            const ::testing::ScopedTrace trace(where.file_name(), static_cast<int>(where.line()), "paneOverWall");

            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            const std::array pane{
                osg::Vec3f(-20.0f, -50.0f, -20.0f),
                osg::Vec3f(20.0f, -50.0f, -20.0f),
                osg::Vec3f(20.0f, -50.0f, 20.0f),
                osg::Vec3f(-20.0f, -50.0f, 20.0f),
            };

            SceneDesc scene = makeWall();
            place(scene, pane);

            const osg::Vec3f bright(2.0f, 2.0f, 2.0f);
            const Shaders::VisibilityConstants camera = lookAtIt
                ? wallCamera(size, bright, osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, -50.0f, 0.0f))
                : wallCamera(size, bright);

            std::vector<std::uint8_t> pixels;
            EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);
            return { pixels[centre], pixels[centre + 1], pixels[centre + 2] };
        }

        /// What the last `countHits` traced, in linear radiance, for a test that wants the figure
        /// rather than the byte.
        ///
        /// **Filled by `countHits` and by nothing else**, because that is the helper whose bytes a
        /// test then holds this against. `renderRadiance` reads into the caller's own vector, so a
        /// read of this after one of those is a read of the frame before it.
        std::vector<float> mRadiance;

    private:
        /// The last frame's radiance, checked against the extent it was drawn at.
        void readRadiance(std::uint32_t size, std::vector<float>& values)
        {
            mRenderer->readChannel(Channel::Radiance, values);
            requireFrame(values, size);
        }

        /// The last frame's radiance, as the bytes a test names.
        void encodeRadiance(std::vector<std::uint8_t>& pixels) const
        {
            pixels.resize(mRadiance.size());
            for (std::size_t at = 0; at < mRadiance.size(); at += 4)
            {
                for (std::size_t channel = 0; channel < 3; ++channel)
                    pixels[at + channel] = encodeSrgb(mRadiance[at + channel]);

                // Coverage rather than radiance, which the curve does not touch either.
                pixels[at + 3]
                    = static_cast<std::uint8_t>(std::lround(std::clamp(mRadiance[at + 3], 0.0f, 1.0f) * 255.0f));
            }
        }

        /// The fixture's textures, numbered the way its scene added them.
        ///
        /// **A convention of these tests and not of the renderer.** Every test here builds its
        /// descriptions in the order its scene calls `addTexture`, so position is slot. The
        /// array used to assume that of every caller, which is a trap for the one whose scene
        /// has given a slot back: its table has a hole in it and its descriptions do not.
        ///
        /// **The span reaches into `mNumbered` and the next render overwrites it**, which is safe
        /// because `renderShot` is the one caller and hands it straight to `setScene`.
        std::span<const TextureData> inSceneOrder(std::span<const TextureData> textures)
        {
            mNumbered.assign(textures.begin(), textures.end());
            for (std::size_t at = 0; at < mNumbered.size(); ++at)
                mNumbered[at].mSlot = static_cast<std::uint32_t>(at);

            return mNumbered;
        }

        std::vector<TextureData> mNumbered;
    };
}

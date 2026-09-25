#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Matrixf>
#include <osg/Vec3f>
#include <osg/Vec4f>

#include <components/rtx/camera.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/brdf.h>
#include <components/rtx/shaders/look.h>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/shaders/visibility.h>
#include <components/rtx/shapefold.hpp>
#include <components/rtx/specularalbedo.hpp>
#include <components/rtx/surface.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtx/textureencoding.hpp>
#include <components/rtx/texturewrap.hpp>
#include <components/vfs/pathutil.hpp>

#include "../geometry.hpp"
#include "../lobeintegrals.hpp"
#include "../testcamera.hpp"
#include "../testtexture.hpp"
#include "fixture.hpp"

namespace Rtx::Testing
{
    namespace
    {
        /// How far the vertex normals of `leaningFloor` lean off their own triangle.
        ///
        /// **Seventy degrees, which is what the content is like.** Four hits in a hundred of a
        /// Morrowind frame carry a shading normal more than sixty degrees off the triangle it sits
        /// on, so this is inside the range the renderer meets rather than a corner built to fail.
        constexpr float sLeaningNormal = 70.0f * std::numbers::pi_v<float> / 180.0f;

        /// A level sheet at the origin whose vertex normals all lean `sLeaningNormal` off it.
        SceneDesc leaningFloor()
        {
            SceneDesc scene;

            const osg::Vec3f leaning(std::sin(sLeaningNormal), 0.0f, std::cos(sLeaningNormal));
            const std::array<osg::Vec3f, 4> normals{ leaning, leaning, leaning, leaning };

            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(MeshArrays{
                    .mPositions = sheetAt(4000.0f, 0.0f), .mNormals = normals, .mIndices = sQuadIndices }) });

            return scene;
        }

        /// The sun, which is one direction everywhere and casts a shadow to the end of the world.
        ///
        /// The wall's normal is (0, -1, 0), so a sun travelling straight along it meets it square and
        /// the whole answer is the irradiance: 0.5 albedo times 2.0 over pi is 0.318310 linear, which
        /// encodes to 1.055 * 0.318310^(1/2.4) - 0.055 = 0.599797, or 153 of 255.
        TEST_F(RtxVisibilityTest, theSunLightsWhatItFacesAndTheOccluderTakesItAway)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            const Shaders::VisibilityConstants base = Testing::makeCamera(
                osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            const std::array occluder{
                osg::Vec3f(-10.0f, -25.0f, -10.0f),
                osg::Vec3f(10.0f, -25.0f, -10.0f),
                osg::Vec3f(10.0f, -25.0f, 10.0f),
                osg::Vec3f(-10.0f, -25.0f, 10.0f),
            };

            // **A sky rather than the cell's ambient, because that is what fills a wall now.** The
            // ambient terminates a path one bounce further along; what a surface the eye can see
            // gathers is its own hemisphere, and a sky of one radiance makes that gather exact —
            // every direction returns the same number, so one sample is the whole answer.
            const auto render = [&](const osg::Vec3f& direction, const osg::Vec3f& irradiance, bool blocked,
                                    const osg::Vec3f& sky = osg::Vec3f()) {
                SceneDesc scene = makeWall();
                if (blocked)
                    scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(MeshArrays{ .mPositions = occluder, .mIndices = sQuadIndices }) });

                Shaders::VisibilityConstants camera = base;
                camera.mSun = Shaders::sunSource(-direction, irradiance);
                camera.mSkyHorizon = sky;
                camera.mSkyZenith = sky;
                camera.mAmbientFromSky = 1.0f;

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);
                return pixels[centre];
            };

            // Travelling along +Y, which is straight into the wall's face.
            const osg::Vec3f onto(0.0f, 1.0f, 0.0f);
            const osg::Vec3f bright(2.0f, 2.0f, 2.0f);

            EXPECT_EQ(render(onto, bright, false), 153) << "square to the sun";
            EXPECT_EQ(render(onto, bright, true), 0) << "and with something standing in the way";

            // A sun travelling out of the wall rather than into it reaches its back, and is dropped
            // rather than arithmetically applied. Asserted against the sky, because a negative
            // contribution clamps to black as well and the two only tell apart against something:
            // 0.5 * 0.4 = 0.2 linear, which encodes to 124 of 255.
            EXPECT_EQ(render(-onto, bright, false, osg::Vec3f(0.4f, 0.4f, 0.4f)), 124)
                << "a sun behind the wall lights nothing";

            // Half the irradiance is nowhere near half the byte, because the encoding is not
            // linear: 0.5 * 1.0 / pi = 0.159155, which encodes to 0.435542, or 111 of 255.
            EXPECT_EQ(render(onto, osg::Vec3f(1.0f, 1.0f, 1.0f), false), 111) << "and half as much sun";

            // At sixty degrees off square the cosine is exactly a half, so this is the same radiance
            // the half-irradiance case gave — the same 111, reached the other way.
            const osg::Vec3f slanted(std::sqrt(3.0f) * 0.5f, 0.5f, 0.0f);
            EXPECT_EQ(render(slanted, bright, false), 111) << "or the same again from a slant";
        }

        /// The eye sees through the nearest pane to what stands behind it.
        ///
        /// **A translucent surface used to be the hit**, resolved against the stand-in cutoff
        /// `AlphaMode::Blend` is given, so a pane with an opaque texture was drawn solid and whatever
        /// was behind it was never traced at all. It is now shaded, kept, and the ray carries on from
        /// where it stood.
        ///
        /// A black pane is what makes the arithmetic checkable: it is lit to nothing, so what comes
        /// back is the wall behind it times what the pane let past. At a half that is half the wall's
        /// radiance, which the test above pins at 111 by halving the sun instead — the same number
        /// by the other route, and 0 under the behaviour this replaces.
        TEST_F(RtxVisibilityTest, theEyeSeesThroughTheNearestPaneToWhatStandsBehindIt)
        {
            // **On the ray and off the sun's.** The eye stands at x=100 and looks at the origin, so
            // halfway to the wall its ray is at x=50 — and the sun travels along +Y, so what this
            // pane shadows is the strip of wall at x between 30 and 70 rather than the origin the
            // centre pixel is looking at. The wall it is held against is fully lit.
            const std::array pane{
                osg::Vec3f(30.0f, -50.0f, -20.0f),
                osg::Vec3f(70.0f, -50.0f, -20.0f),
                osg::Vec3f(70.0f, -50.0f, 20.0f),
                osg::Vec3f(30.0f, -50.0f, 20.0f),
            };

            const osg::Vec3f bright(2.0f, 2.0f, 2.0f);
            const auto black = [](float opacity) { return osg::Vec4f(0.0f, 0.0f, 0.0f, opacity); };

            EXPECT_EQ(litThroughPane(pane, std::nullopt, bright), 153) << "the wall alone";
            EXPECT_EQ(litThroughPane(pane, black(1.0f), bright), 0)
                << "a pane that is all there is not translucent, and it is black";

            // Half the wall's radiance, which is where a sun of half the irradiance lands.
            EXPECT_EQ(litThroughPane(pane, black(0.5f), bright), 111) << "and half of the wall through half a pane";

            EXPECT_EQ(litThroughPane(pane, black(0.0f), bright), 153)
                << "a pane that stops nothing is a pane that is not there";
        }

        /// Every layer of a stack is peeled, and not only the nearest of them.
        ///
        /// **A person is a stack.** A cuirass over a skirt over a leg is three surfaces on one
        /// pixel, so an actor under Chameleon — or fading out at the edge of `actors processing
        /// range` — showed its nearest layer see-through and every layer under it at full strength.
        /// `PEEL_LAYERS` is what the launch now peels, and the layer past it is drawn as the solid
        /// it stands in for rather than as a hole through the world.
        ///
        /// Black panes again, for the reason the test above gives: each is lit to nothing, so what
        /// comes back is the wall times what every layer let past. Half a pane a layer halves it a
        /// layer, which is the same multiplication as a sun of half the irradiance — so each of
        /// these is checked against the sun that lands where it lands.
        TEST_F(RtxVisibilityTest, everyLayerOfAStackIsPeeledAndNotOnlyTheNearest)
        {
            const osg::Vec3f bright(2.0f, 2.0f, 2.0f);
            const osg::Vec4f half(0.0f, 0.0f, 0.0f, 0.5f);

            const std::array<osg::Vec4f, 5> stack{ half, half, half, half, half };
            const auto through
                = [&](std::size_t layers) { return litThroughStack(std::span(stack).first(layers), bright); };

            EXPECT_EQ(through(0), 153) << "the wall alone, as the tests above have it";
            EXPECT_EQ(through(1), 111) << "half the wall through one half pane";

            // A quarter and an eighth, which under the one-layer peel this replaces were nought:
            // the second pane was drawn as the solid it is not.
            EXPECT_EQ(through(2), litThroughStack({}, bright * 0.25f));
            EXPECT_EQ(through(3), litThroughStack({}, bright * 0.125f));
            EXPECT_EQ(through(4), litThroughStack({}, bright * 0.0625f));
            EXPECT_GT(through(4), 0) << "four layers is what the budget peels, and it is not black";

            // **The budget's own edge.** The fifth surface is past what the launch peels, so it is
            // shaded as the solid it stands in for — a black one — rather than left as a hole.
            EXPECT_EQ(through(5), 0);
        }

        /// **A shell is one layer of the peel and not two.**
        ///
        /// The rasterizer draws the world with `GL_CULL_FACE` on, so a robe, a cuirass and a pane
        /// of glass each show the eye one face. Culling nothing, the peel walked through the far
        /// wall as well as the near one and composited the same alpha twice: the Ancestor Ghost,
        /// whose seven body shapes blend `SRC_ALPHA, INV_SRC_ALPHA` at an alpha of 0.45 and carry no
        /// `NiStencilProperty`, covered 0.45 + 0.55 * 0.45 of its pixel where the game covers 0.45.
        /// A faded actor was worse: `PEEL_LAYERS` is four, counted for a cuirass over a skirt over a
        /// leg, and with the backs of those three the budget ran out and the last layer drew solid.
        ///
        /// Two black half panes on the eye's ray, the near one facing the eye and the far one facing
        /// away, which is what a closed surface is from outside. The figures are the ones the test
        /// above pins: one layer is 111 and two is the wall at a quarter.
        TEST_F(RtxVisibilityTest, aShellIsPeeledFromTheFaceItShowsAndNotFromItsBack)
        {
            constexpr std::uint32_t size = 33;

            const osg::Vec3f bright(2.0f, 2.0f, 2.0f);
            const osg::Vec4f half(0.0f, 0.0f, 0.0f, 0.5f);

            // Where `litThroughStack` stands its panes: the eye is at x = 100 looking at the origin,
            // so its ray runs at x = -y and a pane on it is centred there.
            const auto paneAt = [](float away, bool facingTheEye) {
                std::array<osg::Vec3f, 4> pane = uprightQuadAt(10.0f, away);
                for (osg::Vec3f& corner : pane)
                    corner.x() -= away;

                return facingTheEye ? pane : turned(pane);
            };

            const auto throughShell = [&](bool bothFaces) {
                SceneDesc scene = makeWall();
                addPane(scene, paneAt(-60.0f, true), half);
                addPane(scene, paneAt(-50.0f, false), half, 1.0f, bothFaces);

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(countHits(scene, {}, wallCamera(size, bright), size, pixels), 0u);

                return int{ pixels[centreValueOf(size)] };
            };

            EXPECT_EQ(throughShell(false), 111) << "the far wall of a shell was drawn";
            EXPECT_EQ(throughShell(true), int{ litThroughStack({}, bright * 0.25f) })
                << "and a surface the content draws from both faces is still two layers";
        }

        /// A placement the game is fading is seen through, whatever its material says.
        ///
        /// **The same pane, made see-through by the other of the two numbers.** The tests around
        /// this one give the material an alpha. This one leaves the material fully opaque and fades
        /// the placement instead, which is where `MWRender::TransparencyUpdater` puts an actor's
        /// distance fade, Invisibility and Chameleon. The picture has to be the same: the shader
        /// multiplies the two, and nothing downstream of that knows which one it came from.
        ///
        /// **The pane carries no texture**, which is the case this most needs to be right about. A
        /// faded placement is forced non-opaque whatever its material, so it reaches the cutout test
        /// — and a material with no mask has nothing there for that test to read.
        TEST_F(RtxVisibilityTest, aFadedPlacementIsSeenThroughWhateverItsMaterialSays)
        {
            const std::array pane{
                osg::Vec3f(30.0f, -50.0f, -20.0f),
                osg::Vec3f(70.0f, -50.0f, -20.0f),
                osg::Vec3f(70.0f, -50.0f, 20.0f),
                osg::Vec3f(30.0f, -50.0f, 20.0f),
            };

            const osg::Vec3f bright(2.0f, 2.0f, 2.0f);
            const osg::Vec4f black(0.0f, 0.0f, 0.0f, 1.0f);

            EXPECT_EQ(litThroughPane(pane, black, bright), 0) << "a black pane nothing is fading";

            // The same 111 the two tests around this one reach, by the third of the three routes to
            // it: half the wall through a pane the game is showing half of.
            EXPECT_EQ(litThroughPane(pane, black, bright, 0.5f), 111);

            EXPECT_EQ(litThroughPane(pane, black, bright, 0.0f), 153) << "an actor faded away entirely";
        }

        /// A translucent occluder dims the sun rather than stopping it.
        ///
        /// **The cheap half of transparency, and the reason it is cheap is order.** A shadow ray's
        /// answer is a product of what it passed through, and a product does not care which factor
        /// came first — so this needs no sorting, no layer budget and no second pass, where the eye
        /// needs all three.
        ///
        /// Measured against the neighbouring test's own numbers rather than against a byte worked out
        /// here: the tone curve is not the sRGB encode, so half a radiance is not half a byte. What
        /// *is* exact is that a half-transmitting pane and a half-bright sun are the same
        /// multiplication, so they have to land on the same pixel.
        TEST_F(RtxVisibilityTest, aTranslucentOccluderDimsTheSunRatherThanStoppingIt)
        {
            // Square in the sun's path to the middle of the wall, which is where the centre pixel
            // looks — so what this pane changes is the light arriving rather than the view of it.
            const std::array occluder{
                osg::Vec3f(-10.0f, -25.0f, -10.0f),
                osg::Vec3f(10.0f, -25.0f, -10.0f),
                osg::Vec3f(10.0f, -25.0f, 10.0f),
                osg::Vec3f(-10.0f, -25.0f, 10.0f),
            };

            const osg::Vec3f bright(2.0f, 2.0f, 2.0f);
            const auto white = [](float opacity) { return osg::Vec4f(1.0f, 1.0f, 1.0f, opacity); };

            EXPECT_EQ(litThroughPane(occluder, std::nullopt, bright), 153)
                << "square to the sun, as the tests above say";
            EXPECT_EQ(litThroughPane(occluder, white(1.0f), bright), 0)
                << "a pane that is all there is not translucent at all";

            // Half the sun through the pane is the same multiplication as half a sun with none, and
            // the tests above pin that at 111.
            EXPECT_EQ(litThroughPane(occluder, white(0.5f), bright), 111) << "and half of it through half a pane";
            EXPECT_EQ(litThroughPane(occluder, std::nullopt, osg::Vec3f(1.0f, 1.0f, 1.0f)), 111)
                << "which is where it came from";

            // A pane that stops nothing is a pane that is not there.
            EXPECT_EQ(litThroughPane(occluder, white(0.0f), bright), 153);
        }

        /// A glow, which the engine treats as two different things and so does this.
        ///
        /// The emissive **colour** joins the light and is multiplied by the texture, so a surface
        /// glows *with its own texture in it*. The emissive **map** is added past the albedo, so it
        /// glows through whatever the surface is made of. Getting either backwards is visible: a
        /// mushroom cap comes out flat white, or a map on a coloured surface goes black.
        TEST_F(RtxVisibilityTest, aGlowJoinsTheLightAndAGlowingMapIsAddedPastIt)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            // **Six of 255 on the map and a fortieth as the colour, so both land inside the display's
            // range once `EMISSIVE_INTENSITY` has scaled them.** The bytes are derived from the
            // scale below rather than pinned, because the scale is the renderer's to move; what is
            // pinned is that the two paths differ only in whether the albedo stands between.
            const std::array<std::uint8_t, 4> white{ 255, 255, 255, 255 };
            const std::array<std::uint8_t, 4> green{ 0, 255, 0, 255 };
            const std::array<std::uint8_t, 4> dimRed{ 6, 0, 0, 255 };

            const std::array<TextureData, 3> textures{ describeTexel(white), describeTexel(green),
                describeTexel(dimRed) };

            const std::array positions = cardAt(0.0f);

            // Nothing lights this scene at all: no lamp, no sun, no ambient. Whatever comes back is
            // the surface's own glow and nothing else.
            const Shaders::VisibilityConstants camera = Testing::makeCamera(
                osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            const auto render = [&](Index diffuse, Index emissiveMap, const osg::Vec3f& emissiveColour) {
                SceneDesc scene;
                const Index mesh = scene.addMesh(
                    MeshArrays{ .mPositions = positions, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
                scene.textures().add(VFS::Path::NormalizedView("white.dds"));
                scene.textures().add(VFS::Path::NormalizedView("green.dds"));
                scene.textures().add(VFS::Path::NormalizedView("red.dds"));

                const Index material = scene.addMaterial(Material{
                    .mDiffuse = diffuse,
                    .mEmissive = emissiveMap,
                    .mEmissiveColour = emissiveColour,
                });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, textures, camera, size, pixels), size * size);
                return std::array<std::uint8_t, 3>{ pixels[centre], pixels[centre + 1], pixels[centre + 2] };
            };

            // A glow on a white surface, taken so that the product is 0.4 linear whatever the scale
            // is — which encodes to `1.055 * 0.4^(1/2.4) - 0.055 = 0.66514`, or 170. The scale
            // carries the original's "one is a fully lit surface" onto this renderer's, and what is
            // pinned here is the encoding and not the scale.
            const float glow = 0.4f / Shaders::EMISSIVE_INTENSITY;
            const std::uint8_t glowing = encodeSrgb(glow * Shaders::EMISSIVE_INTENSITY);
            EXPECT_EQ(glowing, 170) << "the glow was meant to sit inside the display's range";

            const osg::Vec3f chosen(glow, glow, glow);
            EXPECT_EQ(render(0, sNoIndex, chosen)[0], glowing) << "a glow on white";

            // The same glow on a texture with no red in it keeps the texture's colour, because the
            // glow goes through the albedo. Added past it, the surface would come back white.
            const std::array<std::uint8_t, 3> onGreen = render(1, sNoIndex, chosen);
            EXPECT_EQ(onGreen[1], glowing) << "the same glow, still through green";
            EXPECT_EQ(onGreen[0], 0) << "and with none of the red the white one had";

            // The map is the other way round: red light off a green surface. Through the albedo it
            // would be black, since green times red is nothing. Six of 255 is 0.023529, times eight
            // is 0.188235, which encodes to 120 — reached without the texture's help.
            const std::uint8_t mapGlow = encodeSrgb(6.0f / 255.0f * Shaders::EMISSIVE_INTENSITY);
            EXPECT_EQ(mapGlow, 120);

            const std::array<std::uint8_t, 3> mapped = render(1, 2, osg::Vec3f());
            EXPECT_EQ(mapped[0], mapGlow) << "the map's own red, undimmed by the green under it";
            EXPECT_EQ(mapped[1], 0) << "and none of the green, which nothing is lighting";
        }

        /// A masked surface in front of a wall: the ray stops on what survives the cutout and goes
        /// on through what does not.
        ///
        /// The scene is arranged so the answer is a whole number of pixels. The mask is sixteen
        /// texels across, red throughout, and transparent over its left half; the quad carrying it
        /// exactly fills a sixty-degree frame at a hundred units, so image column c samples
        /// u = (c + 0.5) / 64 and the sampler's REPEAT wrap makes both seams behave the same way.
        /// At column 31 the filter sits 0.375 of the way onto the first opaque texel and at column
        /// 32 it sits 0.625 on, so a cutoff of 0.5 falls exactly between them with a margin of
        /// 0.125 either side — far wider than the four bits of sub-texel precision Vulkan
        /// guarantees. The left half of the image is therefore the wall behind and the right half
        /// is the mask, with nothing in between.
        TEST_F(RtxVisibilityTest, aCutoutStopsARayOnItsMaskAndLetsItThroughTheHoles)
        {
            constexpr std::uint32_t size = 64;
            constexpr std::uint32_t extent = 16;
            constexpr std::uint32_t seam = size / 2;

            std::vector<std::uint8_t> bytes(std::size_t{ extent } * extent * 4);
            for (std::uint32_t y = 0; y < extent; ++y)
                for (std::uint32_t x = 0; x < extent; ++x)
                {
                    std::uint8_t* const texel = &bytes[(std::size_t{ y } * extent + x) * 4];
                    texel[0] = 255;
                    texel[3] = x < extent / 2 ? 0 : 255;
                }

            const MipLevel level{ 0, extent, extent };
            const TextureData data{
                .mFormat = TextureFormat::Rgba8Unorm,
                .mWidth = extent,
                .mHeight = extent,
                .mBytes = std::as_bytes(std::span(bytes)),
                .mLevels = std::span(&level, 1),
            };

            const std::span<const TextureData> textures(&data, 1);

            const std::array masked = cardAt(-50.0f);

            Shaders::VisibilityConstants camera = Testing::makeCamera(
                osg::Vec3f(0.0f, -150.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mShow = Shaders::SHOW_ALBEDO;

            const auto render = [&](AlphaMode mode, float alphaRef) {
                SceneDesc scene = makeWall();
                const Index mesh = scene.addMesh(
                    MeshArrays{ .mPositions = masked, .mTexCoords = sQuadUv, .mIndices = sQuadIndices });
                const Index material = scene.addMaterial(Material{
                    .mDiffuse = scene.textures().add(VFS::Path::NormalizedView("mask.dds")),
                    .mAlphaRef = alphaRef,
                    .mAlphaMode = mode,
                });
                scene.addInstance(
                    MeshInstance{ .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = material });

                std::vector<std::uint8_t> pixels;
                // Something is behind every hole, so every ray lands on one surface or the other.
                EXPECT_EQ(countHits(scene, textures, camera, size, pixels), size * size);
                return pixels;
            };

            // Red where the mask survived and grey where the wall shows through: the mask is pure
            // red, so its green is zero, and the untextured wall's albedo of 0.5 encodes to
            // 1.055 * 0.5^(1/2.4) - 0.055 = 0.73536, or 187.5 of 255 — which is why the grey is the
            // one value here given a byte of room.
            constexpr int wallGrey = 188;

            const std::vector<std::uint8_t> cutout = render(AlphaMode::Cutout, 0.5f);
            ASSERT_EQ(cutout.size(), std::size_t{ size } * size * 4);
            for (std::uint32_t row = 0; row < size; ++row)
                for (std::uint32_t column = 0; column < size; ++column)
                {
                    const std::uint8_t* const pixel = &cutout[(std::size_t{ row } * size + column) * 4];
                    if (column >= seam)
                    {
                        ASSERT_EQ(pixel[0], 255) << "red at " << column << ", " << row;
                        ASSERT_EQ(pixel[1], 0) << "green at " << column << ", " << row;
                    }
                    else
                    {
                        ASSERT_NEAR(pixel[0], wallGrey, 1) << "red at " << column << ", " << row;
                        ASSERT_NEAR(pixel[1], wallGrey, 1) << "green at " << column << ", " << row;
                    }
                }

            // A blend that named no threshold of its own is traced against the stand-in, and the
            // stand-in is the same half. Same bytes, or Morrowind's foliage — which is blended and
            // never alpha-tested — would not be cut out at all.
            EXPECT_EQ(render(AlphaMode::Blend, 0.0f), cutout);

            // And the control: the same texture on an opaque material hides the wall completely, so
            // it is the cutout doing this and not the geometry.
            const std::vector<std::uint8_t> opaque = render(AlphaMode::Opaque, 0.5f);
            for (std::size_t i = 0; i < opaque.size(); i += 4)
            {
                ASSERT_EQ(opaque[i], 255) << "red at pixel " << i / 4;
                ASSERT_EQ(opaque[i + 1], 0) << "green at pixel " << i / 4;
            }
        }

        /// A wall lit by one lamp, at the radiance the falloff says and nowhere else.
        ///
        /// The centre pixel looks straight at the origin, where the wall's normal is (0, -1, 0) and
        /// the light sits fifty units along it, so the cosine is exactly one and the whole answer is
        /// the falloff. Written out, with a reach of 500 and the untextured albedo of 0.5:
        ///
        ///   window    = 1 - (50 / 500)^4              = 0.99990
        ///   falloff   = window^2 / (50^2 + 1)         = 0.99980 / 2501 = 3.99760e-4
        ///   radiance  = 4000 * falloff / pi           = 0.508947
        ///   encoded   = 1.055 * (0.5 * 0.508947)^(1/2.4) - 0.055 = 0.54150, or 138 of 255
        ///
        /// The camera stands off the light's axis so that something can be put between the wall and
        /// the lamp without also standing in front of the wall.
        ///
        /// That cosine of one is also what pins the normal: it holds only if the plane's normal came
        /// back out of the acceleration structure unrotated and unmirrored, which symmetrical
        /// geometry otherwise hides well enough to survive being looked at.
        TEST_F(RtxVisibilityTest, aLampLightsAWallByItsFalloffAndAnObstacleTakesItAway)
        {
            // Odd, so one pixel sits exactly on the axis and its ray lands exactly on the origin.
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            const Shaders::VisibilityConstants base = Testing::makeCamera(
                osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            // Ten units across, a quarter of the way from the wall to the lamp: it covers the whole
            // shadow ray and none of the camera's, which passes through x = 25 at that height.
            const std::array occluder{
                osg::Vec3f(-10.0f, -25.0f, -10.0f),
                osg::Vec3f(10.0f, -25.0f, -10.0f),
                osg::Vec3f(10.0f, -25.0f, 10.0f),
                osg::Vec3f(-10.0f, -25.0f, 10.0f),
            };

            // A sky rather than the cell's ambient, for the reason the sun's own test gives: what
            // fills a wall the eye can see is the hemisphere it gathers.
            const auto render = [&](const std::optional<Light>& light, const osg::Vec3f& sky, bool blocked) {
                SceneDesc scene = makeWall();
                if (light.has_value())
                    scene.addLight(*light);
                if (blocked)
                    scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(MeshArrays{ .mPositions = occluder, .mIndices = sQuadIndices }) });

                Shaders::VisibilityConstants camera = base;
                camera.mSkyHorizon = sky;
                camera.mSkyZenith = sky;
                camera.mAmbientFromSky = 1.0f;

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);
                return pixels[centre];
            };

            const Light lamp{
                .mPosition = osg::Vec3f(0.0f, -50.0f, 0.0f),
                .mIntensity = osg::Vec3f(4000.0f, 4000.0f, 4000.0f),
                .mReach = 500.0f,
            };

            EXPECT_EQ(render(lamp, osg::Vec3f(), false), 138) << "lit by the lamp alone";

            // The same lamp with something in the way. Nothing else lights the wall, so the pixel
            // goes to black rather than merely dimmer — which is what tells a shadow from a falloff.
            EXPECT_EQ(render(lamp, osg::Vec3f(), true), 0) << "and shadowed by the quad between them";

            // Ambient with no lamp at all: 0.5 * 0.4 = 0.2 linear, which encodes to
            // 1.055 * 0.2^(1/2.4) - 0.055 = 0.48453, or 124 of 255.
            const osg::Vec3f sky(0.4f, 0.4f, 0.4f);
            EXPECT_EQ(render(std::nullopt, sky, false), 124) << "the sky alone";

            // A lamp behind the wall meets it at a cosine of minus one, and is dropped rather than
            // arithmetically applied. Asserted against the sky and not against black, because
            // black is what a negative contribution clamps to as well — the two only tell apart
            // where there is something for it to be subtracted from.
            Light behind = lamp;
            behind.mPosition = osg::Vec3f(0.0f, 50.0f, 0.0f);
            EXPECT_EQ(render(behind, sky, false), 124) << "a lamp on the far side lights nothing";

            // The window, biting at half the reach rather than at the very end of it, where it is
            // indistinguishable from the inverse square alone:
            //
            //   window   = 1 - (50 / 100)^4      = 0.93750
            //   falloff  = 0.87891 / 2501        = 3.51422e-4
            //   radiance = 4000 * falloff / pi   = 0.447465
            //   encoded  = 1.055 * (0.5 * 0.447465)^(1/2.4) - 0.055 = 0.51035, or 130 of 255
            Light near = lamp;
            near.mReach = 100.0f;
            EXPECT_EQ(render(near, osg::Vec3f(), false), 130) << "the window taking a bite out of it";

            // And the reach as a hard limit: at exactly its own reach a lamp is skipped outright.
            Light spent = lamp;
            spent.mReach = 50.0f;
            EXPECT_EQ(render(spent, osg::Vec3f(), false), 0) << "and one whose reach ends at the wall";
        }

        /// **A wall with a specular map reflects the lamp by the lobe the host evaluates**, a
        /// dielectric keeps what the lobe did not take for its diffuse half, and a normal map turns
        /// both through the tangent frame the rasterizer builds.
        ///
        /// The lamp test's wall, lamp and camera, with a metal and roughness map on the wall and the
        /// sky black, so the centre pixel is the lamp's direct light and nothing else: its
        /// irradiance square to the wall, `4000 * 3.99760e-4 = 1.59904`, through `brdf.h` at the
        /// pixel's own vectors — the eye along `(1, -1, 0) / √2` and the lamp along `(0, -1, 0)`.
        /// The base colour and the roughness are both a byte of 128, `0.501961`, read linearly as a
        /// data map is.
        ///
        /// **The normal map is the frame's test.** The mesh carries the tangent `(1, 0, 0)` with a
        /// handedness of one and the normal `(0, -1, 0)`, so the bitangent `cross(N, T) * w` is
        /// `(0, 0, 1)`; a texel of `(191, 128, 221)` is the tangent-space normal
        /// `(0.498, 0.0039, 0.733)`, which leans the normal toward the eye's side of the wall. A
        /// frame built another way round — a bitangent flipped, a tangent read as the bitangent —
        /// leans it somewhere else, and every term below moves with it.
        ///
        /// **A vertex tint darkens both halves**, and the reflectance's edge with it: a tint of a
        /// quarter takes the dielectric's 4% to 1%, whose edge is `50 * 0.01 = 0.5`.
        ///
        /// **What the device is held to is the host's arithmetic, to float rounding**: the same
        /// scalar functions and the same table lookup, `SpecularAlbedo::at`. A lobe off by a factor
        /// anywhere is off by far more than a part in ten thousand. The surface views are held to
        /// the same inputs exactly: the mapped normal, the painted roughness and the reflectance.
        TEST_F(RtxVisibilityTest, aSpecularMapReflectsTheLampByTheHostsLobe)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            constexpr std::array<std::uint8_t, 4> sBaseTexel{ 128, 128, 128, 255 };
            constexpr std::array<std::uint8_t, 4> sLeaningTexel{ 191, 128, 221, 255 };
            const Shaders::VisibilityConstants camera = Testing::makeCamera(
                osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            const float irradiance = 4000.0f * 3.99760e-4f;
            const float base = 128.0f / 255.0f;
            const float roughness = 128.0f / 255.0f;
            const osg::Vec3f toEye = osg::Vec3f(1.0f, -1.0f, 0.0f) / std::sqrt(2.0f);
            const osg::Vec3f toLamp(0.0f, -1.0f, 0.0f);
            const osg::Vec3f normal(0.0f, -1.0f, 0.0f);
            const osg::Vec3f tangent(1.0f, 0.0f, 0.0f);

            const std::array<osg::Vec3f, 4> normals{ normal, normal, normal, normal };
            const std::array<osg::Vec4f, 4> tangents{ osg::Vec4f(tangent, 1.0f), osg::Vec4f(tangent, 1.0f),
                osg::Vec4f(tangent, 1.0f), osg::Vec4f(tangent, 1.0f) };

            const auto lit
                = [&](std::uint8_t metal, bool leaning, std::uint32_t show = Shaders::SHOW_SHADED, float tint = 1.0f) {
                      const std::array<std::uint8_t, 4> mapTexel{ metal, 128, 255, 255 };
                      const std::array<TextureData, 3> textures{ describeTexel(sBaseTexel, 0),
                          describeTexel(mapTexel, 1), describeTexel(sLeaningTexel, 2) };
                      const osg::Vec3f colour(tint, tint, tint);
                      const std::array<osg::Vec3f, 4> colours{ colour, colour, colour, colour };

                      SceneDesc scene;
                      const Index mesh = scene.addMesh(MeshArrays{ .mPositions = sWallQuad,
                          .mNormals = normals,
                          .mTexCoords = sQuadUv,
                          .mColours = colours,
                          .mTangents = tangents,
                          .mIndices = sQuadIndices });
                      const Index diffuse = scene.textures().add(VFS::Path::NormalizedView("base.dds"));
                      const Index map = scene.textures().add(
                          VFS::Path::NormalizedView("base_spec.dds"), TextureWrap::Repeat, TextureEncoding::Data);
                      const Index normalMap = scene.textures().add(
                          VFS::Path::NormalizedView("base_n.dds"), TextureWrap::Repeat, TextureEncoding::Data);
                      scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                          .mMesh = mesh,
                          .mMaterial = scene.addMaterial(Material{ .mDiffuse = diffuse,
                              .mNormal = leaning ? normalMap : sNoIndex,
                              .mSpecular = map,
                              .mVertexColour = VertexColour::Tint }) });
                      scene.addLight(Light{
                          .mPosition = osg::Vec3f(0.0f, -50.0f, 0.0f),
                          .mIntensity = osg::Vec3f(4000.0f, 4000.0f, 4000.0f),
                          .mReach = 500.0f,
                      });

                      Shaders::VisibilityConstants shown = camera;
                      shown.mShow = show;
                      std::vector<std::uint8_t> pixels;
                      EXPECT_GT(countHits(scene, textures, shown, size, pixels), 0u);
                      return osg::Vec3f(mRadiance[centre], mRadiance[centre + 1], mRadiance[centre + 2]);
                  };

            // The pixel on the host, at a shading normal of `shading`, a metalness of `metal` and a
            // vertex tint of `tint`.
            const auto expected = [&](const osg::Vec3f& shading, float metal, float tint = 1.0f) {
                osg::Vec3f halfway = toEye + toLamp;
                halfway.normalize();
                const float toLight = shading * toLamp;
                const float eyeCosine = shading * toEye;

                const float reflectance = (metal * base + (1.0f - metal) * Shaders::DIELECTRIC_F0) * tint;
                const float alpha = Shaders::ggxAlpha(roughness);
                const osg::Vec2f table = SpecularAlbedo::shared().at(eyeCosine, roughness);
                const float fresnel = Shaders::fresnelSchlick(
                    reflectance, Shaders::specularEdge(reflectance), Shaders::schlickWeight(toEye * halfway));
                const float lobe = fresnel * Shaders::specularCompensation(reflectance, table.y())
                    * Shaders::ggxDistribution(alpha, shading * halfway)
                    * Shaders::smithVisibility(alpha, eyeCosine, toLight) * toLight;
                const float scattered = (1.0f - metal) * base * tint * toLight * Shaders::INV_PI * (1.0f - fresnel);

                return irradiance * (scattered + lobe);
            };

            // A metal: no diffuse half at all, and the base colour is the reflectance.
            const float metal = expected(normal, 1.0f);
            EXPECT_NEAR(lit(255, false).x(), metal, metal * 1e-4f) << "a metal";

            // A dielectric: the base colour scatters what the lobe's Fresnel term did not take, and
            // the lobe reflects 4% and climbing.
            const float dielectric = expected(normal, 0.0f);
            EXPECT_NEAR(lit(0, false).x(), dielectric, dielectric * 1e-4f) << "a dielectric";

            const float tinted = expected(normal, 0.0f, 0.25f);
            EXPECT_NEAR(lit(0, false, Shaders::SHOW_SHADED, 0.25f).x(), tinted, tinted * 1e-4f)
                << "a tinted dielectric";
            EXPECT_EQ(lit(0, false, Shaders::SHOW_SPECULAR, 0.25f), osg::Vec3f(0.01f, 0.01f, 0.01f))
                << "the tint on the reflectance";

            // The same through the leaning map, decoded as `2 * byte / 255 - 1`.
            const osg::Vec3f painted(
                2.0f * 191.0f / 255.0f - 1.0f, 2.0f * 128.0f / 255.0f - 1.0f, 2.0f * 221.0f / 255.0f - 1.0f);
            osg::Vec3f mapped = tangent * painted.x() + (normal ^ tangent) * painted.y() + normal * painted.z();
            mapped.normalize();
            const float leaning = expected(mapped, 0.0f);
            EXPECT_NEAR(lit(0, true).x(), leaning, leaning * 1e-4f) << "through the leaning map";
            EXPECT_GT(std::abs(leaning - dielectric), dielectric * 1e-2f) << "a map that leans nothing";

            // And the views of the same inputs: the mapped normal as `0.5 + 0.5 n`, the painted
            // roughness, and the reflectance of a metal, which is its base colour.
            const osg::Vec3f shownNormal = lit(0, true, Shaders::SHOW_NORMAL);
            const osg::Vec3f wantedNormal = mapped * 0.5f + osg::Vec3f(0.5f, 0.5f, 0.5f);
            for (int axis = 0; axis < 3; ++axis)
                EXPECT_NEAR(shownNormal[axis], wantedNormal[axis], 1e-5f) << axis;
            EXPECT_EQ(lit(0, false, Shaders::SHOW_ROUGHNESS), osg::Vec3f(roughness, roughness, roughness));
            EXPECT_EQ(lit(255, false, Shaders::SHOW_SPECULAR), osg::Vec3f(base, base, base));
        }

        /// **A map that stands in is read as no map**, in every role an object's map has: a normal
        /// map, a specular map, a dark map, an emissive map and an environment sheet. The slot is
        /// described as the stand-in, as the builder describes a file that does not read and as the
        /// backend stands a texture it has no room for, and the lit wall of the test above is then
        /// the same pixel as with no map at all, to the bit. A real map in the same slot moves the
        /// pixel, which is what says each role reaches it.
        ///
        /// **A second wall wears a specular map in every render**, so every render runs the variant
        /// with the maps compiled in and the renders differ by the slot alone. It stands a thousand
        /// units behind the first, where no ray the pixel sends can reach: the bounce leaves the
        /// front face, and the lamp and the eye are on that side too.
        TEST_F(RtxVisibilityTest, aMapThatStandsInIsReadAsNoMap)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            constexpr std::array<std::uint8_t, 4> sBaseTexel{ 128, 128, 128, 255 };
            constexpr std::array<std::uint8_t, 4> sMapTexel{ 64, 96, 160, 255 };
            const Shaders::VisibilityConstants camera = Testing::makeCamera(
                osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            const osg::Vec3f normal(0.0f, -1.0f, 0.0f);
            const osg::Vec4f tangent(1.0f, 0.0f, 0.0f, 1.0f);
            const std::array<osg::Vec3f, 4> normals{ normal, normal, normal, normal };
            const std::array<osg::Vec4f, 4> tangents{ tangent, tangent, tangent, tangent };

            enum class Role
            {
                Normal,
                Specular,
                Dark,
                Emissive,
                Environment,
            };
            enum class Held
            {
                Nothing,
                StandIn,
                Map,
            };

            const auto lit = [&](Role role, Held held) {
                std::array<TextureData, 3> textures{ describeTexel(sBaseTexel, 0), describeTexel(sMapTexel, 1),
                    describeTexel(sMapTexel, 2) };
                if (held == Held::StandIn)
                    textures[1].mSource = TextureSource::StandIn;

                SceneDesc scene;
                const Index mesh = scene.addMesh(MeshArrays{ .mPositions = sWallQuad,
                    .mNormals = normals,
                    .mTexCoords = sQuadUv,
                    .mTangents = tangents,
                    .mIndices = sQuadIndices });
                const bool data = role == Role::Normal || role == Role::Specular;
                const Index diffuse = scene.textures().add(VFS::Path::NormalizedView("base.dds"));
                const Index map = scene.textures().add(VFS::Path::NormalizedView("map.dds"), TextureWrap::Repeat,
                    data ? TextureEncoding::Data : TextureEncoding::Colour);
                const Index behind = scene.textures().add(
                    VFS::Path::NormalizedView("behind_spec.dds"), TextureWrap::Repeat, TextureEncoding::Data);

                Material material{ .mDiffuse = diffuse };
                const Index named = held == Held::Nothing ? sNoIndex : map;
                switch (role)
                {
                    case Role::Normal:
                        material.mNormal = named;
                        break;
                    case Role::Specular:
                        material.mSpecular = named;
                        break;
                    case Role::Dark:
                        material.mDark = named;
                        break;
                    case Role::Emissive:
                        material.mEmissive = named;
                        break;
                    case Role::Environment:
                        material.mEnvironment = named;
                        break;
                }

                scene.addInstance(MeshInstance{
                    .mTransform = osg::Matrixf::identity(), .mMesh = mesh, .mMaterial = scene.addMaterial(material) });
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::translate(0.0f, 1000.0f, 0.0f),
                    .mMesh = mesh,
                    .mMaterial = scene.addMaterial(Material{ .mDiffuse = diffuse, .mSpecular = behind }) });
                scene.addLight(Light{
                    .mPosition = osg::Vec3f(0.0f, -50.0f, 0.0f),
                    .mIntensity = osg::Vec3f(4000.0f, 4000.0f, 4000.0f),
                    .mReach = 500.0f,
                });

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(countHits(scene, textures, camera, size, pixels), 0u);
                return osg::Vec3f(mRadiance[centre], mRadiance[centre + 1], mRadiance[centre + 2]);
            };

            for (const auto& [role, name] :
                { std::pair(Role::Normal, "a normal map"), std::pair(Role::Specular, "a specular map"),
                    std::pair(Role::Dark, "a dark map"), std::pair(Role::Emissive, "an emissive map"),
                    std::pair(Role::Environment, "an environment sheet") })
            {
                const osg::Vec3f none = lit(role, Held::Nothing);
                EXPECT_EQ(lit(role, Held::StandIn), none) << name << " that stands in";
                EXPECT_NE(lit(role, Held::Map), none) << name << " held moves nothing, so this proves nothing";
            }
        }

        /// **A glossy surface holds its lamps by what they send back through the lobe**, and a grey
        /// metal under two lamps is then sampled with no variance at all: its lobe is all it returns,
        /// so a lamp's weight is its term, and whichever lamp is held, the estimate is the two lobes
        /// summed. One frame is the host's sum, to float rounding.
        ///
        /// The wall, camera and metal of the specular test, and two lamps fifty units from the
        /// middle of the wall, each delivering that test's `1.59904` square to its direction: one
        /// along `(0, -1, 0)` and one along `(-0.6, -0.8, 0)`, nearer the eye's mirror direction.
        /// Weighed by the diffuse cosine instead, a frame holding either is that lamp's lobe times
        /// `1.8` over its own cosine — which is the sum only where the second lamp's lobe is 0.8 of
        /// the first's, and at these directions it is more than the first's.
        TEST_F(RtxVisibilityTest, aMetalHoldsTheLampsItsLobeReturnsAndIsSampledWithNoNoise)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            constexpr std::array<std::uint8_t, 4> sBaseTexel{ 128, 128, 128, 255 };
            constexpr std::array<std::uint8_t, 4> sMetalTexel{ 255, 128, 255, 255 };
            const std::array<TextureData, 2> textures{ describeTexel(sBaseTexel, 0), describeTexel(sMetalTexel, 1) };

            const osg::Vec3f normal(0.0f, -1.0f, 0.0f);
            const osg::Vec4f tangent(1.0f, 0.0f, 0.0f, 1.0f);
            const std::array normals{ normal, normal, normal, normal };
            const std::array tangents{ tangent, tangent, tangent, tangent };

            SceneDesc scene;
            const Index mesh = scene.addMesh(MeshArrays{ .mPositions = sWallQuad,
                .mNormals = normals,
                .mTexCoords = sQuadUv,
                .mTangents = tangents,
                .mIndices = sQuadIndices });
            const Index diffuse = scene.textures().add(VFS::Path::NormalizedView("base.dds"));
            const Index map = scene.textures().add(
                VFS::Path::NormalizedView("base_spec.dds"), TextureWrap::Repeat, TextureEncoding::Data);
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = mesh,
                .mMaterial = scene.addMaterial(Material{ .mDiffuse = diffuse, .mSpecular = map }) });

            const osg::Vec3f toFirst(0.0f, -1.0f, 0.0f);
            const osg::Vec3f toSecond(-0.6f, -0.8f, 0.0f);
            for (const osg::Vec3f& towards : { toFirst, toSecond })
                scene.addLight(Light{
                    .mPosition = towards * 50.0f,
                    .mIntensity = osg::Vec3f(4000.0f, 4000.0f, 4000.0f),
                    .mReach = 500.0f,
                });

            const Shaders::VisibilityConstants camera = Testing::makeCamera(
                osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);

            const float irradiance = 4000.0f * 3.99760e-4f;
            const float reflectance = 128.0f / 255.0f;
            const float roughness = 128.0f / 255.0f;
            const osg::Vec3f toEye = osg::Vec3f(1.0f, -1.0f, 0.0f) / std::sqrt(2.0f);
            const auto lobe = [&](const osg::Vec3f& toLamp) {
                osg::Vec3f halfway = toEye + toLamp;
                halfway.normalize();
                const float toLight = normal * toLamp;
                const float eyeCosine = normal * toEye;
                const float alpha = Shaders::ggxAlpha(roughness);
                const osg::Vec2f table = SpecularAlbedo::shared().at(eyeCosine, roughness);
                const float fresnel = Shaders::fresnelSchlick(
                    reflectance, Shaders::specularEdge(reflectance), Shaders::schlickWeight(toEye * halfway));
                return irradiance * fresnel * Shaders::specularCompensation(reflectance, table.y())
                    * Shaders::ggxDistribution(alpha, normal * halfway)
                    * Shaders::smithVisibility(alpha, eyeCosine, toLight) * toLight;
            };

            const float first = lobe(toFirst);
            const float second = lobe(toSecond);
            ASSERT_GT(second, 0.8f * first) << "the directions no longer tell the two targets apart";

            std::vector<std::uint8_t> pixels;
            EXPECT_GT(countHits(scene, textures, camera, size, pixels), 0u);
            EXPECT_NEAR(mRadiance[centre], first + second, (first + second) * 1e-4f);
        }

        /// Which side of a surface the light may come from is the triangle's plane's answer, and a
        /// vertex normal that disagrees does not get to overrule it.
        ///
        /// **Morrowind's vertex normals point clean through their own triangles.** A stretch of the
        /// floor in the Seyda Neen customs office interpolates to one aimed at the ground, on a quad
        /// whose plane is level to a hundredth — and a normal merely turned to face the *ray* is
        /// left pointing down there, because at a shallow enough view it already does face the eye.
        /// A floor with its normal under it drops every lamp overhead on the cosine and sends its
        /// bounce into itself, which came out as a black band that slid about as the camera moved.
        ///
        /// The wall is met at fourteen degrees to its own plane, which is what makes that possible:
        /// the camera stands at `(200, -50, 0)`, so the ray travels `(-0.970, 0.243, 0)` and a normal
        /// of `(0.6, 0.8, 0)` — pointing through the wall, away from the lamp — still meets it at
        /// `-0.388` and passes for facing it.
        ///
        /// Three renders, and the arithmetic is the falloff from the lamp test with a cosine on it:
        ///
        ///   falloff  = (1 - (50 / 500)^4)^2 / (50^2 + 1)      = 3.99760e-4
        ///   plane    = 4000 * 0.5 * 1.0 * falloff / pi        = 0.254491 -> 138 of 255
        ///   tilted   = 4000 * 0.5 * 0.8 * falloff / pi        = 0.203593 -> 125 of 255
        ///
        /// The tilted normal is *used* — 125 is its own cosine of 0.8 and not the plane's one — and
        /// only which side it sits on is taken from the plane. Turned to face the ray instead it
        /// meets the lamp at minus 0.8 and the pixel is black, which is the whole of the defect.
        TEST_F(RtxVisibilityTest, aVertexNormalMayTiltAShadingModelButNotChooseWhichSideIsLit)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            // Fourteen degrees off the wall's own plane, and the centre ray lands exactly on the
            // origin — so the cosines below are the shading normal's and nothing else's.
            Shaders::VisibilityConstants camera = Testing::makeCamera(
                osg::Vec3f(200.0f, -50.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
            camera.mSkyHorizon = osg::Vec3f();
            camera.mSkyZenith = osg::Vec3f();

            const Light lamp{
                .mPosition = osg::Vec3f(0.0f, -50.0f, 0.0f),
                .mIntensity = osg::Vec3f(4000.0f, 4000.0f, 4000.0f),
                .mReach = 500.0f,
            };

            const auto render = [&](std::span<const osg::Vec3f> normals) {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(
                        MeshArrays{ .mPositions = sWallQuad, .mNormals = normals, .mIndices = sQuadIndices }) });
                scene.addLight(lamp);

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);
                return pixels[centre];
            };

            // No vertex normals at all, so the plane is the whole answer and meets the lamp square.
            EXPECT_EQ(render({}), 138) << "the plane alone";

            // The same quad with a normal tilted through it, which is the case that was black.
            const osg::Vec3f through(0.6f, 0.8f, 0.0f);
            const std::array tilted{ through, through, through, through };
            EXPECT_EQ(render(tilted), 125) << "a normal authored through its own triangle still lights this side";

            // **And the authored side carries no meaning**, which is what makes the plane the only
            // thing deciding: negating every vertex normal is the same surface and has to be the
            // same pixel, to the byte.
            const std::array flipped{ -through, -through, -through, -through };
            EXPECT_EQ(render(flipped), render(tilted)) << "which way the normals were authored is not information";
        }

        /// A leaf is lit through its back, and nothing else is.
        ///
        /// The wall stands square to the camera and the sun is put behind it. A solid takes nothing
        /// from there and the pixel is black; a sheet with a mask takes `SHEET_TRANSMISSION` of
        /// what the same sun would give its front. The sheet's texture is white and whole, so its
        /// albedo is one: the front under an irradiance of two is 2 / pi = 0.63662, and the back is
        /// half that, 0.31831 — the number the sun test reaches for a front of albedo a half. A lamp
        /// behind it is the same arithmetic on the lamp test's falloff: 4000 * 3.99760e-4 / pi
        /// = 0.50898 on the front, and half of it on the back.
        ///
        /// The two controls say which fact each is. The same card not doubled is a solid, and a
        /// doubled card with no mask is cloth: a tabard is seen from the side it is lit from. Both
        /// are black from behind.
        TEST_F(RtxVisibilityTest, aLeafIsLitThroughItsBackAndClothAndSolidsAreNot)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            TestTexture white;
            paintOpaqueSheet(white);
            const std::span<const TextureData> textures(&white.mData, 1);

            const osg::Vec3f behind(0.0f, 50.0f, 0.0f);
            const osg::Vec3f before(0.0f, -50.0f, 0.0f);

            const auto render = [&](bool sheet, bool masked, const osg::Vec3f& lit, bool lamp) {
                SceneDesc scene;

                Material material;
                if (masked)
                {
                    material.mDiffuse = scene.textures().add(VFS::Path::NormalizedView("sheet.dds"));
                    material.mAlphaMode = AlphaMode::Cutout;
                    material.mAlphaRef = 0.5f;
                }

                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(
                        MeshArrays{ .mPositions = sWallQuad, .mTexCoords = sQuadUv, .mIndices = sQuadIndices },
                        FoldedShape{ .mSheet = sheet }),
                    .mMaterial = scene.addMaterial(material) });

                if (lamp)
                    scene.addLight(Light{
                        .mPosition = lit,
                        .mIntensity = osg::Vec3f(4000.0f, 4000.0f, 4000.0f),
                        .mReach = 500.0f,
                    });

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
                camera.mSun
                    = Shaders::sunSource(lit / lit.length(), lamp ? osg::Vec3f() : osg::Vec3f(2.0f, 2.0f, 2.0f));
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(
                    countHits(scene, masked ? textures : std::span<const TextureData>(), camera, size, pixels), 0u);
                return mRadiance[centre];
            };

            const float front = 2.0f * Shaders::INV_PI;

            EXPECT_NEAR(render(true, true, before, false), front, 1e-3f) << "a leaf's front is a Lambert front";
            EXPECT_NEAR(render(true, true, behind, false), front * Shaders::SHEET_TRANSMISSION, 1e-3f)
                << "and its back takes the sun at the transmission";

            EXPECT_EQ(render(false, true, behind, false), 0.0f) << "a solid with the same mask takes nothing";
            EXPECT_EQ(render(true, false, behind, false), 0.0f) << "and neither does cloth: doubled, but no mask";

            const float lamplit = 4000.0f * 3.99760e-4f * Shaders::INV_PI;
            EXPECT_NEAR(render(true, true, before, true), lamplit, 1e-3f) << "a lamp before the leaf";
            EXPECT_NEAR(render(true, true, behind, true), lamplit * Shaders::SHEET_TRANSMISSION, 1e-3f)
                << "is the same lamp behind it, at the transmission";
            EXPECT_EQ(render(false, true, behind, true), 0.0f) << "and a solid takes nothing from it";
        }

        /// A half-plane hung behind the wall, and one column of pixels read against the same column
        /// with nothing in the way.
        ///
        /// **The whole of a soft shadow is where the shadow ray leaves from.** A lamp with an extent
        /// and the sun's half-degree disc are neither of them one direction: the ray is drawn from
        /// somewhere on the source, and the band an occluder hides some of it from is the penumbra.
        /// How wide that band is, is arithmetic — the source's own size seen from the occluder — and
        /// the three tests below pin one source each at both edges of its band and in the middle.
        ///
        /// A half-plane occluder, so the geometry is one number: an edge at `x = X` in a plane
        /// parallel to the wall, hung behind it and so out of the camera's own view. The wall is at
        /// y = 0 and the source is straight out along -Y, so the **centre column** of pixels stands
        /// at x = 0 whatever row it is on — every one of them is at the same place in the penumbra,
        /// and averaging the column is averaging draws of one quantity rather than smearing several.
        /// Each is divided by what the same pixel reads with nothing in the way, so the falloff and
        /// the cosine — which do differ down the column — cancel and what is left is visibility.
        class RtxPenumbraTest : public RtxVisibilityTest
        {
        protected:
            static constexpr std::uint32_t sSize = 33;
            static constexpr std::uint32_t sColumn = sSize / 2;
            static constexpr float sLampDepth = -200.0f;

            /// A quad behind the wall covering everything left of `edge`, wide enough that no shadow
            /// ray this frame sends leaves it by the far side.
            static std::array<osg::Vec3f, 4> halfPlane(float depth, float edge)
            {
                return {
                    osg::Vec3f(edge - 1000.0f, depth, -1000.0f),
                    osg::Vec3f(edge, depth, -1000.0f),
                    osg::Vec3f(edge, depth, 1000.0f),
                    osg::Vec3f(edge - 1000.0f, depth, 1000.0f),
                };
            }

            SceneDesc sceneWith(const std::optional<Light>& lamp, float depth, std::optional<float> edge)
            {
                SceneDesc scene = makeWall();
                if (lamp.has_value())
                    scene.addLight(*lamp);
                if (edge.has_value())
                {
                    const std::array quad = halfPlane(depth, *edge);
                    scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(MeshArrays{ .mPositions = quad, .mIndices = sQuadIndices }) });
                }
                return scene;
            }

            /// The frame with nothing in the way, which every reading below is divided by.
            std::vector<float> openFor(
                const std::optional<Light>& lamp, float depth, const Shaders::VisibilityConstants& camera)
            {
                std::vector<float> open;
                renderRadiance(sceneWith(lamp, depth, std::nullopt), camera, sSize, open, { .mFrames = 1 });
                return open;
            }

            /// What share of the source the centre column can see.
            ///
            /// **One frame where the answer is the same on every frame, and sixty-four where it is a
            /// draw.** A pixel wholly inside or wholly outside the penumbra is decided by geometry
            /// and repeats; one in the middle of it is a coin, and the mean of the column over
            /// sixty-four frames is 33 * 64 = 2112 of them — a standard error of
            /// sqrt(0.25 / 2112) = 0.0109, so a tolerance of 0.05 is four and a half of them.
            float visible(const SceneDesc& scene, const std::vector<float>& open,
                const Shaders::VisibilityConstants& camera, std::uint32_t frames)
            {
                std::vector<float> shadowed;
                renderRadiance(scene, camera, sSize, shadowed, { .mFrames = frames });

                double total = 0.0;
                for (std::uint32_t row = 0; row < sSize; ++row)
                {
                    const std::size_t at = (std::size_t{ row } * sSize + sColumn) * 4;
                    total += static_cast<double>(shadowed[at] / open[at]);
                }
                return static_cast<float>(total / sSize);
            }

            /// Straight on, so the centre column's rays stay in the plane x = 0 and land on the wall
            /// there, which is what makes one column a run of draws of the same quantity.
            static Shaders::VisibilityConstants lookAtTheWall()
            {
                Shaders::VisibilityConstants camera
                    = Testing::makeCamera(osg::Vec3f(0.0f, -100.0f, 0.0f), osg::Vec3f(), 60.0f, sSize, sSize, 10000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                return camera;
            }

            /// Bright enough to read well clear of the quantiser and nowhere near saturating: the
            /// falloff at 400 units is 1 / 160001, so the wall comes back at 400000 / (160001 * pi)
            /// times its own albedo of a half, which is 0.398.
            static Light makeLamp()
            {
                return Light{
                    .mPosition = osg::Vec3f(0.0f, -400.0f, 0.0f),
                    .mIntensity = osg::Vec3f(400000.0f, 400000.0f, 400000.0f),
                    .mReach = 10000.0f,
                    .mSourceRadius = 20.0f,
                    .mClearance = 20.0f,
                };
            }
        };

        /// A source whose size was measured casts a penumbra as wide as that size allows.
        ///
        /// **The lamp** stands 400 units out with a source radius of 20, and the occluder hangs
        /// halfway: a ray leaving a point 20 units off the axis crosses the occluder's plane 10 off
        /// it, so an edge twelve units either side is wholly clear of the cone or wholly across it.
        ///
        ///     half-width = 200 * tan(asin(20 / 400)) = 10.013
        TEST_F(RtxPenumbraTest, aMeasuredSourceCastsAPenumbraAsWideAsItsOwnSizeAllows)
        {
            const Light lamp = makeLamp();
            const Shaders::VisibilityConstants camera = lookAtTheWall();

            const std::vector<float> open = openFor(lamp, sLampDepth, camera);
            ASSERT_GT(open[(std::size_t{ sColumn } * sSize + sColumn) * 4], 0.0f) << "the lamp lights the wall";

            EXPECT_FLOAT_EQ(visible(sceneWith(lamp, sLampDepth, -12.0f), open, camera, 1), 1.0f)
                << "the whole source clears an edge outside its penumbra";
            EXPECT_FLOAT_EQ(visible(sceneWith(lamp, sLampDepth, 12.0f), open, camera, 1), 0.0f)
                << "and none of it clears one across the far side";
            EXPECT_NEAR(visible(sceneWith(lamp, sLampDepth, 0.0f), open, camera, 64), 0.5f, 0.05f)
                << "and exactly half of it stands on the shadow's own edge";
        }

        /// A fill's penumbra is as wide as its whole ball, because from outside it the ray is aimed
        /// anywhere across the ball — and the ball is the clearance, so nothing inside it shadows.
        ///
        /// The same lamp made a fill two hundred across: from the wall the ball subtends
        /// `asin(200 / 400)`, thirty degrees, so a ray to its edge crosses an occluder a quarter of
        /// the way out `100 * tan(30°) = 57.74` off the axis — an edge at 60 either side is wholly
        /// clear of every ray or across every one, and the ball's own symmetry puts half of them
        /// on either side of the axis. A quarter of the way and not halfway, because the ray stops
        /// the ball's radius short of its closest approach to the centre: at twenty degrees off
        /// the axis that is 176 units out, and an occluder at 200 would stand past its end.
        TEST_F(RtxPenumbraTest, aFillCastsAPenumbraAsWideAsItsWholeBall)
        {
            Light fill = makeLamp();
            fill.mSourceRadius = 200.0f;
            fill.mClearance = 200.0f;
            fill.mFill = 1;
            const Shaders::VisibilityConstants camera = lookAtTheWall();
            constexpr float depth = -100.0f;

            const std::vector<float> open = openFor(fill, depth, camera);
            ASSERT_GT(open[(std::size_t{ sColumn } * sSize + sColumn) * 4], 0.0f) << "the fill lights the wall";

            EXPECT_FLOAT_EQ(visible(sceneWith(fill, depth, -60.0f), open, camera, 1), 1.0f)
                << "the whole ball clears an edge outside its penumbra";
            EXPECT_FLOAT_EQ(visible(sceneWith(fill, depth, 60.0f), open, camera, 1), 0.0f)
                << "and none of it clears one across the far side";
            EXPECT_NEAR(visible(sceneWith(fill, depth, 0.0f), open, camera, 64), 0.5f, 0.05f)
                << "and half of it stands on the shadow's own edge";
        }

        /// A floor inside a fill's ball is lit by the ball from every side, and shadowed by nothing.
        ///
        /// **Inside the ball the cosine to the centre is blended out, and no ray is sent.** The
        /// ball's centre stands three hundred units along the floor and half a unit over it, so the
        /// floor's centre meets it at a cosine of 0.001667 — as a lamp, all but nothing — a quarter
        /// of the way inside a ball four hundred across:
        ///
        ///   window   = 1 - (300 / 1600)^4                       = 0.998764
        ///   falloff  = window^2 / (300^2 + 400^2)               = 3.99011e-6
        ///   depth    = 1 - 300 / 400                             = 0.25
        ///   cosine   = mix(0.001667, 1, 0.25)                    = 0.251249
        ///   radiance = 500000 * falloff * cosine / pi            = 0.159554
        ///   encoded  = 1.055 * (0.5 * 0.159554)^(1/2.4) - 0.055  = 0.31286, or 80 of 255
        ///
        /// where the lamp of the same record reads `500000 * falloff * 0.001667 / pi` = 0.001058,
        /// which is 2 of 255. A sheet ten units under the floor takes nothing away, because the
        /// ball is the ray's clearance and the floor stands inside it: drawn to a point anywhere on
        /// the ball instead, half of a floor's rays went down through the floor into whatever was
        /// under it, and came back as a speckle over the whole pool.
        TEST_F(RtxVisibilityTest, aFloorInsideAFillIsLitFromEverySideAndShadowedByNothing)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            const auto render = [&](bool fill, bool underneath) {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh
                    = scene.addMesh(MeshArrays{ .mPositions = sheetAt(4000.0f, 0.0f), .mIndices = sQuadIndices }) });
                if (underneath)
                    scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                        .mMesh = scene.addMesh(
                            MeshArrays{ .mPositions = sheetAt(4000.0f, -10.0f), .mIndices = sQuadIndices }) });
                scene.addLight(Light{
                    .mPosition = osg::Vec3f(300.0f, 0.0f, 0.5f),
                    .mIntensity = osg::Vec3f(500000.0f, 500000.0f, 500000.0f),
                    .mReach = 1600.0f,
                    .mSourceRadius = 400.0f,
                    .mClearance = 400.0f,
                    .mFill = fill ? 1u : 0u,
                });

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -100.0f, 100.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbientFromSky = 1.0f;

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);
                return pixels[centre];
            };

            EXPECT_EQ(render(false, false), 2) << "the lamp of the same record, nearly level with the floor";
            EXPECT_EQ(render(true, false), 80) << "the fill, a quarter of the way inside its ball";
            EXPECT_EQ(render(true, true), 80) << "and the same floor with a sheet under it";
        }

        /// Outside its ball a fill is exactly the lamp of its record.
        ///
        /// A thousand units off, facing the centre, a fill four hundred across reads `falloff`'s
        /// inverse square softened by that four hundred, the same as a lamp whose flame is four
        /// hundred across would:
        ///
        ///   window   = 1 - (1000 / 1600)^4                       = 0.847412
        ///   falloff  = window^2 / (1000^2 + 400^2)               = 6.19058e-7
        ///   radiance = 600000 * falloff / pi                     = 0.118233
        ///   encoded  = 1.055 * (0.5 * 0.118233)^(1/2.4) - 0.055  = 0.26967, or 69 of 255
        TEST_F(RtxVisibilityTest, aFillOutsideItsBallIsTheLampOfItsRecord)
        {
            constexpr std::uint32_t size = 33;
            constexpr std::size_t centre = centreValueOf(size);

            const auto render = [&](bool fill) {
                SceneDesc scene = makeWall();
                scene.addLight(Light{
                    .mPosition = osg::Vec3f(0.0f, -1000.0f, 0.0f),
                    .mIntensity = osg::Vec3f(600000.0f, 600000.0f, 600000.0f),
                    .mReach = 1600.0f,
                    .mSourceRadius = 400.0f,
                    .mClearance = 400.0f,
                    .mFill = fill ? 1u : 0u,
                });

                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(100.0f, -100.0f, 0.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 10000.0f);
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbientFromSky = 1.0f;

                std::vector<std::uint8_t> pixels;
                EXPECT_GT(countHits(scene, {}, camera, size, pixels), 0u);
                return pixels[centre];
            };

            EXPECT_EQ(render(false), 69) << "the lamp";
            EXPECT_EQ(render(true), 69) << "and the fill of the same record";
        }

        /// A source carrying no size casts an edge, inside the band a measured one is still part-lit
        /// across.
        ///
        /// `Rtx::Light::mSourceRadius` says which lamps carry one and which do not. At `X = -1` the
        /// unmeasured lamp is still fully lit and at `X = +1` fully dark, where the sized one is
        /// part-lit at both — the segment of a disc of radius 10.013 cut one unit off its centre:
        ///
        ///     u        = -1 / 10.013                          = -0.09987
        ///     lit      = (acos(u) - u * sqrt(1 - u^2)) / pi    = 0.56348
        TEST_F(RtxPenumbraTest, anUnmeasuredSourceCastsAnEdgeWhereAMeasuredOneIsStillPartLit)
        {
            const Light lamp = makeLamp();
            const Shaders::VisibilityConstants camera = lookAtTheWall();

            Light point = lamp;
            point.mSourceRadius = 0.0f;
            point.mClearance = 0.0f;

            // **Its own open reading, and it has to be its own.** A size softens the singularity in
            // `falloff` as well as widening the shadow ray, so the two lamps do not deliver the same
            // light at the same place — 400 units out, one divides by `400^2 + 20^2` and the other
            // by `400^2 + 1`. Divided by the sized lamp's reading, a point that clears every ray
            // would read 1.0025 rather than one.
            const std::vector<float> pointOpen = openFor(point, sLampDepth, camera);

            EXPECT_FLOAT_EQ(visible(sceneWith(point, sLampDepth, -1.0f), pointOpen, camera, 1), 1.0f)
                << "an unmeasured source is lit right up to its shadow";
            EXPECT_FLOAT_EQ(visible(sceneWith(point, sLampDepth, 1.0f), pointOpen, camera, 1), 0.0f)
                << "and dark from there on, with no band in between";

            const std::vector<float> lampOpen = openFor(lamp, sLampDepth, camera);

            EXPECT_NEAR(visible(sceneWith(lamp, sLampDepth, -1.0f), lampOpen, camera, 64), 0.56348f, 0.05f)
                << "where a measured source is still inside its own penumbra at both";
        }

        /// The sun is one direction everywhere, so its penumbra grows with nothing but the occluder's
        /// distance — two thousand units of it, at the two degrees the shadow cone is drawn from,
        /// which `SUN_SHADOW_RADIUS` says is wider than the disc and why.
        ///
        ///     half-width = 2000 * tan(0.034907) = 69.84
        TEST_F(RtxPenumbraTest, theSunsPenumbraGrowsWithTheOccludersDistanceAlone)
        {
            constexpr float sunDepth = -2000.0f;

            Shaders::VisibilityConstants camera = lookAtTheWall();
            // The disc stands along -Y, so its light travels +Y and meets the wall's face square.
            camera.mSun = Shaders::sunSource(osg::Vec3f(0.0f, -1.0f, 0.0f), osg::Vec3f(2.0f, 2.0f, 2.0f));

            const std::vector<float> open = openFor(std::nullopt, sunDepth, camera);
            ASSERT_GT(open[(std::size_t{ sColumn } * sSize + sColumn) * 4], 0.0f) << "the sun lights the wall";

            EXPECT_FLOAT_EQ(visible(sceneWith(std::nullopt, sunDepth, -72.0f), open, camera, 1), 1.0f)
                << "the whole cone clears an edge outside its penumbra";
            EXPECT_FLOAT_EQ(visible(sceneWith(std::nullopt, sunDepth, 72.0f), open, camera, 1), 0.0f)
                << "and none of it clears one across the far side";
            EXPECT_NEAR(visible(sceneWith(std::nullopt, sunDepth, -10.0f), open, camera, 64), 0.59109f, 0.05f)
                << "and the disc's own half degree is well inside the band: the segment cut ten off "
                   "a cone of 69.84, u = -0.1432, (acos(u) - u * sqrt(1 - u^2)) / pi";

            EXPECT_NEAR(visible(sceneWith(std::nullopt, sunDepth, 0.0f), open, camera, 64), 0.5f, 0.05f)
                << "and half a disc stands on the shadow's own edge, two thousand units back";
        }

        /// What terminates a path is occluded on both sides of a door, and only the reach differs.
        ///
        /// **The first level was always occluded and the second never was.** A bounce ray that hits
        /// something is shaded there and the path stops, and what it was handed was the open sky
        /// whatever stood over it — so a hollow was lit as though the sky reached into it.
        /// `ambientReaching` is the missing half, and `mAmbientFromSky` says how far it looks: out
        /// of doors the ambient is the sky and the ray runs to it, and in a room the ambient is the
        /// `AMBI` fill, which the walls make rather than block, so only what is within
        /// `ROOM_FILL_REACH` takes it away.
        ///
        /// A floor under a lid, lit by nothing but the ambient. Every bounce off the floor lands on
        /// the lid's underside, and what that underside is handed is the whole of the claim.
        ///
        /// **In a room it goes as the square of the height, which is Malley's method read
        /// backwards.** A cosine-drawn direction has `d.z = sqrt(1 - u)`, so `P(d.z >= c) = 1 - c^2`;
        /// a ray from the lid reaches the floor within `r` exactly where `d.z >= h / r`, so what is
        /// left unblocked is `(h / r)^2`. At 35 and 70 units under a reach of 140 that is 0.0625 and
        /// 0.25 — a factor of four for a doubling — and past 140 nothing is blocked at all.
        ///
        /// The lid is 8000 across, so an escape past its edge is 2.2% at 600 units and nothing worth
        /// naming at 70: `tan(theta) > 4000 / 600` is `d.z < 0.1474`, and `0.1474^2` is that.
        TEST_F(RtxVisibilityTest, aRoomsFillIsTakenAwayByWhatIsNearAndAnExteriorsSkyByAnything)
        {
            constexpr std::uint32_t size = 32;

            const auto lidAt = [](float lid) {
                SceneDesc scene;
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh
                    = scene.addMesh(MeshArrays{ .mPositions = sheetAt(4000.0f, 0.0f), .mIndices = sQuadIndices }) });
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh
                    = scene.addMesh(MeshArrays{ .mPositions = sheetAt(4000.0f, lid), .mIndices = sQuadIndices }) });

                return scene;
            };

            // Nothing but the ambient, so what the floor shows is the path's own end and no more.
            const auto floorUnderTheLid = [&](const SceneDesc& scene, float fromSky, float lid) {
                // Between the two, looking down, so the eye finds the floor and the floor's own
                // bounce finds the lid.
                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -1.0f, 0.5f * lid), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mSun.mIrradiance = osg::Vec3f();
                camera.mAmbient = osg::Vec3f(0.5f, 0.5f, 0.5f);
                camera.mAmbientFromSky = fromSky;

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mFrames = 48 });

                return meanRadiance();
            };

            const SceneDesc open = lidAt(600.0f);

            const float near = floorUnderTheLid(lidAt(35.0f), 0.0f, 35.0f);
            const float mid = floorUnderTheLid(lidAt(70.0f), 0.0f, 70.0f);
            const float clear = floorUnderTheLid(open, 0.0f, 600.0f);

            ASSERT_GT(near, 0.0f) << "a room's fill still reaches the second level";

            EXPECT_NEAR(mid / near, 4.0f, 0.4f) << "twice the height is four times the fill";
            EXPECT_NEAR(clear / mid, 3.91f, 0.4f) << "and past the reach it is whole, less the edge";

            // **The sky is occluded by anything at all**, which is the same lid at the same height
            // asked the other question: the underside sees the floor and no sky, so out of doors the
            // floor goes dark where in a room it kept most of its fill.
            EXPECT_LT(floorUnderTheLid(open, 1.0f, 600.0f), 0.1f * clear) << "and the sky does not reach under a lid";
        }

        /// Which side of a surface a light is on is its triangle's answer and never its normal's.
        ///
        /// **A sheet has no thickness for a shadow ray to stop in.** A floor whose vertex normals
        /// lean seventy degrees still faces a sun *below* it — the cosine against the shading normal
        /// is `cos 40` — and the shadow ray it then buys leaves through the floor, meets nothing,
        /// and reports the sun as fully visible. The floor lights itself from underneath.
        ///
        /// The same quad and the same normal twice, with the sun mirrored about the floor's plane.
        /// Above it, the floor takes it at the shading normal's own cosine, which here is one; below
        /// it, the floor takes nothing at all.
        ///
        /// **Both halves are the assertion.** The plane deciding alone would be satisfied by a
        /// renderer that had dropped the shading normal and taken the plane's cosine instead — which
        /// is `cos 70`, a third of what the sun above has to deliver, and nowhere near the tolerance.
        TEST_F(RtxVisibilityTest, aLightBehindASurfacesOwnTriangleDoesNotReachIt)
        {
            constexpr std::uint32_t size = 16;
            constexpr float sunlight = 4.0f;

            const SceneDesc scene = leaningFloor();

            const auto litFrom = [&](float upward) {
                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -1.0f, 300.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

                // Nothing but the sun, so what the floor shows is that one term.
                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mAmbientFromSky = 1.0f;

                camera.mSun
                    = Shaders::sunSource(osg::Vec3f(std::sin(sLeaningNormal), 0.0f, upward * std::cos(sLeaningNormal)),
                        osg::Vec3f(sunlight, sunlight, sunlight));

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mFrames = 16 });

                return meanRadiance();
            };

            // The sun along the shading normal itself: cosine one, albedo a half, and the Lambert
            // divisor. 0.5 * 4 / pi = 0.63662.
            const float above = litFrom(1.0f);
            EXPECT_NEAR(above, 0.5f * sunlight / std::numbers::pi_v<float>, 0.002f)
                << "the shading normal still says how much";

            // Mirrored: `cos 40` against the shading normal, and behind the plane.
            EXPECT_LT(litFrom(-1.0f), 0.002f) << "and the triangle says which side";
        }

        /// A bounce does not gather through the triangle it left.
        ///
        /// **The same leaning normal, asked of the indirect term.** A cosine lobe about a normal
        /// seventy degrees off its plane puts `(1 - cos 70) / 2` of its projected measure *under*
        /// the surface — a third of it — and on sheet geometry those rays meet nothing to stop them.
        /// So the floor gathered whatever stood beneath the floor.
        ///
        /// A glowing sheet on one side of the floor and then the other, with nothing else in the
        /// scene. Above, the floor gathers `(1 + cos 70) / 2` of it, which is the form factor of a
        /// half-space seen at that tilt. Below, it gathers none.
        ///
        /// The sheet's own radiance is `0.5 * 0.25 * EMISSIVE_INTENSITY`, which is one, so the floor
        /// above comes to `0.5 * 0.67101`.
        TEST_F(RtxVisibilityTest, aBounceDoesNotGatherThroughTheTriangleItLeft)
        {
            constexpr std::uint32_t size = 32;

            const auto glowingAt = [](float z) {
                SceneDesc scene = leaningFloor();

                Material glowing;
                glowing.mEmissiveColour = osg::Vec3f(0.25f, 0.25f, 0.25f);

                // Wide enough that every direction off the floor which is on its side meets it, so
                // the share below is the geometry's and not the sheet's edge.
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(MeshArrays{ .mPositions = sheetAt(40000.0f, z), .mIndices = sQuadIndices }),
                    .mMaterial = scene.addMaterial(glowing) });

                return scene;
            };

            // Between the two, looking down, so the floor is what the eye finds either way.
            const auto floorUnder = [&](const SceneDesc& scene) {
                Shaders::VisibilityConstants camera = Testing::makeCamera(
                    osg::Vec3f(0.0f, -1.0f, 50.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);

                camera.mSkyHorizon = osg::Vec3f();
                camera.mSkyZenith = osg::Vec3f();
                camera.mSun.mIrradiance = osg::Vec3f();
                camera.mAmbient = osg::Vec3f();
                camera.mAmbientFromSky = 1.0f;

                std::vector<std::uint8_t> pixels;
                countHits(scene, {}, camera, size, pixels, { .mFrames = 64 });

                return meanRadiance();
            };

            const float share = 0.5f * (1.0f + std::cos(sLeaningNormal));

            EXPECT_NEAR(floorUnder(glowingAt(100.0f)), 0.5f * share, 0.01f) << "the lobe is still the shading normal's";

            EXPECT_LT(floorUnder(glowingAt(-100.0f)), 0.005f) << "and it stops at the triangle";
        }

        /// A bounce is drawn by the cosine, and two thirds is the number that says so.
        ///
        /// **The one property of the estimator a uniform sky cannot show.** Every other test here
        /// fills the sky with one radiance so that a single sample carries no variance — which is
        /// what makes them exact, and what leaves `cosineDirection` unmeasured. A sky that runs from
        /// horizon to zenith turns the direction itself into the answer.
        ///
        /// Malley's method draws `d.z = sqrt(1 - u)` for uniform `u`, so
        ///
        ///   E[d.z]   = integral of sqrt(t) over [0, 1]  = 2/3
        ///   Var[d.z] = E[1 - u] - (2/3)^2 = 1/2 - 4/9   = 1/18,  sd = 0.235702
        ///
        /// A floor of albedo 0.5 under `mix(horizon, zenith, d.z)` therefore has to come back at
        /// `0.5 * (horizon + 2/3 * range)` per channel, spread by `0.5 * |range| * 0.235702`.
        ///
        /// **The mean is also what tells this estimator from the wrong one.** Drawing uniformly over
        /// the hemisphere and carrying the cosine as a weight is unbiased as well, but its
        /// directions average `E[d.z] = 1/2` — a picture a sixth of the sky's range away, which is
        /// ten times the tolerance below and cannot be mistaken for it.
        TEST_F(RtxVisibilityTest, aBounceDrawsItsDirectionByTheCosineAndNotUniformly)
        {
            constexpr std::uint32_t size = 64;
            constexpr float samples = float{ size } * size;

            SceneDesc scene;
            scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                .mMesh = scene.addMesh(MeshArrays{ .mPositions = sheetAt(4000.0f, 0.0f), .mIndices = sQuadIndices }) });

            // Three ranges rather than one, and one of them descending, so a test that passed by
            // matching a total or by swapping two channels would not.
            const osg::Vec3f horizon(0.20f, 0.15f, 0.60f);
            const osg::Vec3f zenith(0.80f, 0.65f, 0.15f);

            // As near straight down as `makeCamera` will take, so every ray lands on the floor and
            // every pixel is one bounce off a normal of +z with nothing else in the frame. Nothing
            // stands above the floor either, so every bounce escapes and the sky is the whole answer.
            Shaders::VisibilityConstants camera = Testing::makeCamera(
                osg::Vec3f(0.0f, -1.0f, 300.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 60.0f, size, size, 100000.0f);
            camera.mSkyHorizon = horizon;
            camera.mSkyZenith = zenith;
            camera.mAmbientFromSky = 1.0f;

            const auto shade = [&](std::uint32_t frame, std::uint32_t accumulate = 0) {
                camera.mFrame = frame;

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, {}, camera, size, pixels, { .mFrames = accumulate }), size * size);
                return pixels;
            };

            // The mean and the standard deviation of one channel across the frame, in linear.
            const auto measure = [&](const std::vector<std::uint8_t>& pixels, std::size_t channel) {
                float sum = 0.0f;
                float squares = 0.0f;
                for (std::size_t i = channel; i < pixels.size(); i += 4)
                {
                    const float linear = decodeSrgb(pixels[i]);
                    sum += linear;
                    squares += linear * linear;
                }

                const float mean = sum / samples;
                return std::pair{ mean, std::sqrt(squares / samples - mean * mean) };
            };

            const std::vector<std::uint8_t> first = shade(0);
            ASSERT_EQ(first.size(), std::size_t{ size } * size * 4);

            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto [mean, spread] = measure(first, channel);
                const float range = zenith[channel] - horizon[channel];
                const float byTheCosine = 0.5f * (horizon[channel] + range * 2.0f / 3.0f);
                const float ifDrawnEvenly = 0.5f * (horizon[channel] + range * 0.5f);

                // One sRGB step at a quarter brightness is 0.004 of linear — a 255th divided by the
                // curve's slope there — and it swamps the sampling standard error, which over 4096
                // samples is `0.5 * |range| * 0.235702 / 64`, at most 0.0011.
                EXPECT_NEAR(mean, byTheCosine, 0.004f) << "channel " << channel << " averages two thirds up";
                EXPECT_GT(std::abs(mean - ifDrawnEvenly), 0.02f)
                    << "channel " << channel << " is nowhere near the half an even draw would give";

                EXPECT_NEAR(spread, 0.5f * std::abs(range) * 0.235702f, 0.003f)
                    << "channel " << channel << " is spread by the square root's own variance";
            }

            // The frame index has to move every pixel's draw, or a bounce would be a fixed pattern
            // that no amount of accumulation could average away. Two independent samples land on the
            // same byte only by coincidence — green's range covers some eighty of them here, so a
            // few per cent — and the rest must differ.
            const std::vector<std::uint8_t> second = shade(1);
            std::size_t moved = 0;
            for (std::size_t i = 1; i < first.size(); i += 4)
                moved += first[i] != second[i] ? 1u : 0u;

            EXPECT_GT(moved, std::size_t{ size } * size * 9 / 10) << "the frame redraws the bounce";

            // **And what the accumulator is for: the error falls as the square root of the count.**
            // Averaging sixty-four independent draws divides the standard deviation of each by eight
            // and leaves the mean where it was — the whole basis for calling a long run a reference,
            // and worth asserting rather than assuming, because a sum that dropped or double-counted
            // a frame would still look converged.
            //
            // **And what the accumulator is for: averaging drives the error down and leaves the mean
            // alone.** Every pixel here has the same normal under the same sky, so its true value is
            // the same number — which makes the spread across the frame the error itself, and its
            // fall with the count the whole basis for calling a long run a reference.
            //
            // **The reduction is bounded at both ends, and both ends are derived.** Independent
            // draws would divide the standard deviation by `sqrt(64)`, which is eight, and nothing
            // can do worse than that — so a floor of eight catches a sum that dropped frames or a
            // turn that stopped turning, either of which leaves a pixel's samples repeating and the
            // spread where one frame left it. The ceiling is sixty-four, the reduction a perfectly
            // stratified sequence would reach, and it catches the opposite fault: a tile that failed
            // to upload reads as zero everywhere, every pixel draws the same direction as every
            // other, and the spread collapses to nothing while the mean stays right.
            //
            // Measured, the reduction is 17, 27 and 21 — comfortably past what independence gives,
            // because the frames are a golden-ratio sweep of the interval rather than sixty-four
            // guesses at it.
            //
            // **The mean's tolerance is twenty times tighter than the single-frame one above**, and
            // has to be: at sixty-four samples a pixel's values cluster inside a few bytes, so the
            // sRGB step that dominated there is no longer what limits this. A divisor off by one
            // moves the mean by 0.0046 — the whole point of the assertion, and something a tolerance
            // sized for one noisy frame would wave through.
            constexpr std::uint32_t averaged = 64;
            const std::vector<std::uint8_t> converged = shade(0, averaged);

            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto [mean, spread] = measure(converged, channel);
                const float range = zenith[channel] - horizon[channel];

                EXPECT_NEAR(mean, 0.5f * (horizon[channel] + range * 2.0f / 3.0f), 0.0002f)
                    << "channel " << channel << " keeps its mean when averaged";

                const float alone = 0.5f * std::abs(range) * 0.235702f;
                EXPECT_GT(alone / spread, std::sqrt(float{ averaged }))
                    << "channel " << channel << " converges at least as fast as independent draws";
                EXPECT_LT(alone / spread, float{ averaged })
                    << "channel " << channel << " converges no faster than a perfect sweep";
            }
        }

        /// **A glossy floor under an even sky gives back what its two halves reflect toward the eye**,
        /// whichever half each pixel's bounce drew: the white furnace, for the specular bounce.
        ///
        /// The cosine test's floor and eye with the view narrowed to two degrees, so every pixel sees
        /// the floor within 1.6 degrees of square on, under a sky of one radiance `L` and nothing
        /// else: every bounce escapes, and a pixel's expected value is `L` times what the surface
        /// reflects toward the eye. Read as the radiance rather than bytes, over four frames averaged.
        ///
        /// **The lobe reflects what `lobeIntegrals` finds square on, compensated off the table.** The
        /// draw is the lobe at a cosine of one, where its whole is 0.91440; the compensation is read
        /// off the table's node at a cosine of one, blended across two roughness rows, and holds
        /// 0.91442. The expectation carries their ratio, which is 2.3e-5 under one.
        ///
        /// - **A white metal**, `F0 = F90 = 1` and no diffuse half, reflects all of what reaches it:
        ///   its compensation is `1 / whole`, so the frame averages `L` to that ratio. Every pixel
        ///   draws the lobe, so this holds the visible-normal draw, its `G2 / G1` and the
        ///   compensation. Uncompensated it averages `L whole`, 0.043 lower; drawn into the indirect
        ///   term, the lobe's light is multiplied by a diffuse albedo of nought.
        /// - **Half a metal**, `m = 128 / 255`: `F0 = 0.04 + 0.96 m` and `c_diff = 1 - m`, and each
        ///   half drawn about as often. The lobe reflects its `Es`, from the integrals. The diffuse half
        ///   reflects `c_diff (1 - F)` averaged over the cosine, which square on is closed: the half
        ///   vector to a direction `θ` off the normal is `θ / 2` off it, so with `c = cos(θ / 2)`,
        ///   `cosθ = 2c² - 1` and `sinθ dθ = -4c dc`, the average is `8 ∫ (1 - F(c)) (2c³ - c) dc`
        ///   over `[1/√2, 1]`. The constant part is `(1 - F0)`, since `∫ (2c³ - c) dc = 1/8`; Schlick's
        ///   takes `8 (F90 - F0) J` off it, with `x = 1 - c` and
        ///   `J = ∫ x⁵ (1 - 5x + 6x² - 2x³) dx` over `[0, 1 - 1/√2]`, 1.0236e-5. A lobe whose weight
        ///   left out its chance — the draw's `1 / p` — would average 0.12 lower.
        ///
        /// **2.5e-3 is four standard errors of independent draws.** One frame's pixels spread by 0.135
        /// and 0.162 about their means, so 65536 draws put a mean within 6.3e-4 of its expectation a
        /// standard error. The directions are blue noise and the choice of half a hash: measured, the
        /// white metal lands within 3e-5 and half a metal within 2.6e-4. The rule's own error is
        /// 1e-5, and the pixels' cosines, 0.9996 at the corners, move the lobe's whole by 8e-6 on
        /// average.
        TEST_F(RtxVisibilityTest, aGlossyFloorUnderAnEvenSkyGivesBackWhatItReflects)
        {
            constexpr std::uint32_t size = 128;
            constexpr float sky = 0.5f;
            constexpr std::uint8_t sRoughness = 128;
            constexpr std::array<std::uint8_t, 4> sWhite{ 255, 255, 255, 255 };

            Shaders::VisibilityConstants camera = Testing::makeCamera(
                osg::Vec3f(0.0f, -1.0f, 300.0f), osg::Vec3f(0.0f, 0.0f, 0.0f), 2.0f, size, size, 100000.0f);
            camera.mSkyHorizon = osg::Vec3f(sky, sky, sky);
            camera.mSkyZenith = osg::Vec3f(sky, sky, sky);
            camera.mAmbientFromSky = 1.0f;

            // The red channel's mean across the frame.
            const auto shade = [&](std::uint8_t metal) {
                const std::array<std::uint8_t, 4> mapTexel{ metal, sRoughness, 255, 255 };
                const std::array<TextureData, 2> textures{ describeTexel(sWhite, 0), describeTexel(mapTexel, 1) };

                SceneDesc scene;
                const Index diffuse = scene.textures().add(VFS::Path::NormalizedView("white.dds"));
                const Index map = scene.textures().add(
                    VFS::Path::NormalizedView("white_spec.dds"), TextureWrap::Repeat, TextureEncoding::Data);
                scene.addInstance(MeshInstance{ .mTransform = osg::Matrixf::identity(),
                    .mMesh = scene.addMesh(MeshArrays{
                        .mPositions = sheetAt(4000.0f, 0.0f), .mTexCoords = sQuadUv, .mIndices = sQuadIndices }),
                    .mMaterial = scene.addMaterial(Material{ .mDiffuse = diffuse, .mSpecular = map }) });

                std::vector<std::uint8_t> pixels;
                EXPECT_EQ(countHits(scene, textures, camera, size, pixels, { .mFrames = 4 }), size * size);

                double sum = 0.0;
                for (std::size_t at = 0; at < mRadiance.size(); at += 4)
                    sum += static_cast<double>(mRadiance[at]);

                return sum / (double{ size } * size);
            };

            const float roughness = static_cast<float>(sRoughness) / 255.0f;
            const osg::Vec2f table = SpecularAlbedo::shared().at(1.0f, roughness);
            const osg::Vec2f squareOn = lobeIntegrals(1.0f, roughness);

            const double white = shade(255);
            const double whole = squareOn.y();
            EXPECT_NEAR(white, double{ sky } * whole / double{ table.y() }, 2.5e-3)
                << "a white metal gives the sky back whole";
            EXPECT_GT(std::abs(white - double{ sky } * whole), 0.04) << "and not the lobe's albedo uncompensated";

            const float metal = 128.0f / 255.0f;
            const float reflectance = Shaders::DIELECTRIC_F0 + (1.0f - Shaders::DIELECTRIC_F0) * metal;
            const float edge = Shaders::specularEdge(reflectance);
            const double scattering = 1.0 - double{ metal };

            const double tail = 1.0 - 1.0 / std::numbers::sqrt2;
            const double squared = tail * tail;
            const double fifth = squared * squared * tail;
            const double integral = fifth * tail / 6.0 - 5.0 * fifth * squared / 7.0
                + 6.0 * fifth * squared * tail / 8.0 - 2.0 * fifth * squared * squared / 9.0;
            const double diffuse = scattering
                * ((1.0 - double{ reflectance }) - 8.0 * (double{ edge } - double{ reflectance }) * integral);
            const double specular
                = static_cast<double>(Shaders::specularAlbedoOf(reflectance, edge, squareOn.x(), squareOn.y())
                    * Shaders::specularCompensation(reflectance, table.y()));
            const double chance = specular / (specular + scattering);

            const double half = shade(128);
            EXPECT_NEAR(half, double{ sky } * (diffuse + specular), 2.5e-3) << "half a metal gives back both halves";
            EXPECT_GT(std::abs(half - double{ sky } * (diffuse + specular * chance)), 0.1)
                << "and its lobe is divided by the chance it was drawn with";
        }
    }
}

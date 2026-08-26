#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/camera.hpp>
#include <components/rtx/instancerecord.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>

#include "harness.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sExtent = 8;

        constexpr std::uint32_t packColour(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha)
        {
            return static_cast<std::uint32_t>(red) | (static_cast<std::uint32_t>(green) << 8)
                | (static_cast<std::uint32_t>(blue) << 16) | (static_cast<std::uint32_t>(alpha) << 24);
        }

        /// Two triangles of a rectangle in clip space, given MyGUI's orientation: `top` is the
        /// coordinate nearer +1.
        std::array<GuiVertex, 6> makeQuad(float left, float top, float right, float bottom, std::uint32_t colour)
        {
            const GuiVertex topLeft{ left, top, 0.0f, colour, 0.0f, 0.0f };
            const GuiVertex topRight{ right, top, 0.0f, colour, 1.0f, 0.0f };
            const GuiVertex bottomLeft{ left, bottom, 0.0f, colour, 0.0f, 1.0f };
            const GuiVertex bottomRight{ right, bottom, 0.0f, colour, 1.0f, 1.0f };

            return { topLeft, bottomLeft, bottomRight, topLeft, bottomRight, topRight };
        }

        /// The GUI as the renderer offers it: a table of textures and one call that draws with them.
        ///
        /// **No scene, and that is the point.** A main menu and a loading screen are drawn over a
        /// frame nothing traced, so everything here has to work before `setScene` has been called
        /// even once.
        class RtxGuiDrawTest : public ::testing::Test
        {
        protected:
            void SetUp() override
            {
                std::string reason;
                mRenderer = Testing::getRenderer(reason);
                if (mRenderer == nullptr)
                    GTEST_SKIP() << reason;

                mRenderer->resize(sExtent, sExtent);

                std::vector<std::string> stale;
                mRenderer->takeValidationErrors(stale);
            }

            void TearDown() override
            {
                if (mRenderer == nullptr)
                    return;

                for (std::uint32_t texture : mHeld)
                    mRenderer->dropGuiTexture(texture);
                mHeld.clear();

                std::vector<std::string> errors;
                mRenderer->takeValidationErrors(errors);
                for (const std::string& error : errors)
                    ADD_FAILURE() << "validation error: " << error;
            }

            /// A one-texel texture of this colour, dropped when the test ends.
            std::uint32_t makeTexel(std::array<std::uint8_t, 4> colour)
            {
                const std::uint32_t texture = mRenderer->addGuiTexture(1, 1);
                mHeld.push_back(texture);
                mRenderer->writeGuiTexture(texture, Renderer::GuiRegion{ 0, 0, 1, 1 }, colour);
                return texture;
            }

            void drawQuad(std::uint32_t texture, float left, float top, float right, float bottom, std::uint32_t colour)
            {
                const std::array<GuiVertex, 6> quad = makeQuad(left, top, right, bottom, colour);
                const std::array<GuiBatch, 1> batches{ GuiBatch{ texture, 0, quad.size() } };
                mRenderer->drawGui(quad, batches);
            }

            /// The four bytes at a pixel of a GUI texture, row zero at the top.
            std::array<std::uint8_t, 4> inTexture(
                std::uint32_t texture, std::uint32_t extent, std::uint32_t x, std::uint32_t y)
            {
                mRenderer->readGuiTexture(texture, mPixels);
                EXPECT_EQ(mPixels.size(), std::size_t{ extent } * extent * 4);

                const std::size_t offset = (static_cast<std::size_t>(y) * extent + x) * 4;
                return { mPixels[offset], mPixels[offset + 1], mPixels[offset + 2], mPixels[offset + 3] };
            }

            /// The four bytes at a pixel of the presented frame, row zero at the top.
            std::array<std::uint8_t, 4> at(std::uint32_t x, std::uint32_t y)
            {
                mRenderer->readPixels(mPixels);
                EXPECT_EQ(mPixels.size(), std::size_t{ sExtent } * sExtent * 4);

                const std::size_t offset = (static_cast<std::size_t>(y) * sExtent + x) * 4;
                return { mPixels[offset], mPixels[offset + 1], mPixels[offset + 2], mPixels[offset + 3] };
            }

            Renderer* mRenderer = nullptr;
            std::vector<std::uint32_t> mHeld;
            std::vector<std::uint8_t> mPixels;
        };

        /// What a batch shows is its texture times its vertex colour, and nothing else.
        TEST_F(RtxGuiDrawTest, aBatchShowsItsTextureTimesItsVertexColour)
        {
            // Halves that divide exactly, so the multiply has nothing to round: 200 × 128/255 is not
            // an integer, but 200 × 1 and 100 × 1 are.
            const std::uint32_t texture = makeTexel({ 200, 100, 50, 255 });

            drawQuad(texture, -1.0f, 1.0f, 1.0f, -1.0f, packColour(255, 255, 255, 255));

            EXPECT_EQ(at(4, 4), (std::array<std::uint8_t, 4>{ 200, 100, 50, 255 }));
        }

        /// A second call draws over what the first left, rather than over whatever the frame held.
        ///
        /// **This is what makes a GUI out of one call per frame.** MyGUI produces its batches in
        /// layer order and the renderer draws them in that order; a pass that discarded the target
        /// each time would show only the last thing drawn.
        TEST_F(RtxGuiDrawTest, aSecondDrawLandsOverTheFirst)
        {
            const std::uint32_t white = makeTexel({ 255, 255, 255, 255 });

            drawQuad(white, -1.0f, 1.0f, 1.0f, -1.0f, packColour(0, 0, 255, 255));
            drawQuad(white, -1.0f, 1.0f, 0.0f, -1.0f, packColour(255, 0, 0, 128));

            // Exact: an alpha of 128/255 contributes 255 × 128/255 = 128 and leaves
            // 255 × 127/255 = 127 of what was there.
            EXPECT_EQ(at(1, 4), (std::array<std::uint8_t, 4>{ 128, 0, 127, 255 })) << "blended over";
            EXPECT_EQ(at(sExtent - 2, 4), (std::array<std::uint8_t, 4>{ 0, 0, 255, 255 })) << "left alone";
        }

        /// Writing a texture again changes what is drawn with it, which is the whole of what a video
        /// frame and a fog of war need.
        TEST_F(RtxGuiDrawTest, rewritingATextureChangesWhatIsDrawn)
        {
            const std::uint32_t texture = makeTexel({ 255, 0, 0, 255 });

            drawQuad(texture, -1.0f, 1.0f, 1.0f, -1.0f, packColour(255, 255, 255, 255));
            EXPECT_EQ(at(4, 4), (std::array<std::uint8_t, 4>{ 255, 0, 0, 255 })) << "as written";

            constexpr std::array<std::uint8_t, 4> sGreen{ 0, 255, 0, 255 };
            mRenderer->writeGuiTexture(texture, Renderer::GuiRegion{ 0, 0, 1, 1 }, sGreen);

            drawQuad(texture, -1.0f, 1.0f, 1.0f, -1.0f, packColour(255, 255, 255, 255));
            EXPECT_EQ(at(4, 4), (std::array<std::uint8_t, 4>{ 0, 255, 0, 255 })) << "as rewritten";
        }

        /// A write of part of a texture changes that part and leaves the rest where it was.
        ///
        /// **What the world map needs and MyGUI's own interface cannot say.** Entering a cell
        /// repaints eighteen pixels square of an overlay two megabytes wide, and sending the whole
        /// of it on the frame the cell arrives is the cost this exists to remove.
        ///
        /// Read back rather than drawn: what is under test is which texels the copy landed on, and a
        /// linear sampler over a four-texel texture blends every one of them into its neighbours.
        ///
        /// **Also what says the two writes happen in the order they were asked for.** Nothing reads
        /// the texture between them, so they share a submit — and copies into one image are
        /// unordered within a submit unless something orders them.
        TEST_F(RtxGuiDrawTest, aRegionWriteChangesItsRectangleAndNothingElse)
        {
            constexpr std::uint32_t side = 4;
            const std::uint32_t texture = mRenderer->addGuiTexture(side, side);
            mHeld.push_back(texture);

            std::array<std::uint8_t, side * side * 4> red{};
            for (std::size_t at = 0; at < red.size(); at += 4)
            {
                red[at] = 255;
                red[at + 3] = 255;
            }
            mRenderer->writeGuiTexture(texture, Renderer::GuiRegion{ 0, 0, side, side }, red);

            // Two texels wide and one tall, at the second column of the second row: a write that
            // ignored the offset, took it as a row count, or transposed it lands somewhere the sweep
            // below looks.
            constexpr std::array<std::uint8_t, 8> green{ 0, 255, 0, 255, 0, 255, 0, 255 };
            mRenderer->writeGuiTexture(texture, Renderer::GuiRegion{ 1, 2, 2, 1 }, green);

            for (std::uint32_t row = 0; row < side; ++row)
                for (std::uint32_t column = 0; column < side; ++column)
                {
                    const bool written = row == 2 && (column == 1 || column == 2);
                    const std::array<std::uint8_t, 4> expected = written
                        ? std::array<std::uint8_t, 4>{ 0, 255, 0, 255 }
                        : std::array<std::uint8_t, 4>{ 255, 0, 0, 255 };

                    EXPECT_EQ(inTexture(texture, side, column, row), expected)
                        << "texel " << column << ", " << row << (written ? " was not written" : " was not left alone");
                }
        }

        /// A slot given back is taken over before the table grows.
        ///
        /// **A session opens and closes menus for hours**, and every window that opens makes
        /// textures. A table that only ever grew would be a slow leak with a number on it.
        TEST_F(RtxGuiDrawTest, aSlotGivenBackIsTakenOverBeforeTheTableGrows)
        {
            const std::uint32_t first = mRenderer->addGuiTexture(1, 1);
            const std::uint32_t second = mRenderer->addGuiTexture(1, 1);
            EXPECT_NE(first, second);

            mRenderer->dropGuiTexture(first);

            const std::uint32_t third = mRenderer->addGuiTexture(1, 1);
            EXPECT_EQ(third, first) << "the freed slot, not a new one";

            mRenderer->dropGuiTexture(second);
            mRenderer->dropGuiTexture(third);
        }

        /// A texture the table has just handed out is blank rather than whatever the memory held.
        ///
        /// **Which is a statement about when the clear runs, not only that it is asked for.** Making
        /// a texture records a clear and submits nothing; this is what says it has run by the time a
        /// draw can name the slot.
        TEST_F(RtxGuiDrawTest, aTextureIsBlankBeforeItIsWritten)
        {
            const std::uint32_t texture = mRenderer->addGuiTexture(1, 1);
            mHeld.push_back(texture);

            const std::uint32_t white = makeTexel({ 255, 255, 255, 255 });
            drawQuad(white, -1.0f, 1.0f, 1.0f, -1.0f, packColour(0, 0, 255, 255));

            // Nothing times anything is nothing: transparent black leaves the blue underneath.
            drawQuad(texture, -1.0f, 1.0f, 1.0f, -1.0f, packColour(255, 255, 255, 255));

            EXPECT_EQ(at(4, 4), (std::array<std::uint8_t, 4>{ 0, 0, 255, 255 }));
        }

        /// Textures made and written before anything reads one each come back holding their own.
        ///
        /// **What the staging buffer is really being asked.** The writes share a submit, so they
        /// share the buffer they are copied out of, a run apiece; three sizes rather than three of
        /// one because a run handed out at the wrong offset only shows where the lengths differ.
        /// Between them they are more than one buffer's worth, so at least one write has to submit
        /// what is pending and start the buffer again — and what was already recorded must still
        /// land.
        TEST_F(RtxGuiDrawTest, texturesWrittenBeforeAnyIsReadEachHoldTheirOwn)
        {
            struct Written
            {
                std::uint32_t mSide;
                std::array<std::uint8_t, 4> mColour;
                std::uint32_t mSlot = 0;
            };

            // A megabyte, a kilobyte and four bytes: the largest is what the staging settles at, and
            // the two after it borrow a corner of what that left.
            std::array<Written, 3> written{
                Written{ .mSide = 512, .mColour = { 255, 0, 0, 255 } },
                Written{ .mSide = 16, .mColour = { 0, 255, 0, 255 } },
                Written{ .mSide = 1, .mColour = { 0, 0, 255, 255 } },
            };

            std::vector<std::uint8_t> rows;
            for (Written& one : written)
            {
                one.mSlot = mRenderer->addGuiTexture(one.mSide, one.mSide);
                mHeld.push_back(one.mSlot);

                rows.clear();
                rows.reserve(std::size_t{ one.mSide } * one.mSide * 4);
                for (std::uint32_t texel = 0; texel < one.mSide * one.mSide; ++texel)
                    rows.insert(rows.end(), one.mColour.begin(), one.mColour.end());

                mRenderer->writeGuiTexture(one.mSlot, Renderer::GuiRegion{ 0, 0, one.mSide, one.mSide }, rows);
            }

            // The corners, because a run that overlapped its neighbour's is wrong at an edge before
            // it is wrong in the middle.
            for (const Written& one : written)
            {
                EXPECT_EQ(inTexture(one.mSlot, one.mSide, 0, 0), one.mColour) << "first texel of " << one.mSide;
                EXPECT_EQ(inTexture(one.mSlot, one.mSide, one.mSide - 1, one.mSide - 1), one.mColour)
                    << "last texel of " << one.mSide;
            }
        }

        /// A texture given back while a write to it is still pending is let go without complaint.
        ///
        /// **The assertion is the validation sweep in `TearDown`.** Nothing has been submitted when
        /// the slot is dropped, so the image being destroyed is one a recorded command still names —
        /// which is a use after free unless what was recorded is submitted first.
        TEST_F(RtxGuiDrawTest, aTextureDroppedWithAWritePendingIsLetGoCleanly)
        {
            const std::uint32_t texture = mRenderer->addGuiTexture(2, 2);

            std::array<std::uint8_t, 2 * 2 * 4> red{};
            for (std::size_t at = 0; at < red.size(); at += 4)
            {
                red[at] = 255;
                red[at + 3] = 255;
            }
            mRenderer->writeGuiTexture(texture, Renderer::GuiRegion{ 0, 0, 2, 2 }, red);

            mRenderer->dropGuiTexture(texture);

            const std::uint32_t again = mRenderer->addGuiTexture(2, 2);
            EXPECT_EQ(again, texture) << "the freed slot, not a new one";
            mRenderer->dropGuiTexture(again);
        }

        /// The GUI over a frame that was actually traced, which is the first time the two halves of
        /// this renderer meet.
        ///
        /// **Nothing here asserts a radiance.** What the trace makes of a bare wall is the trace's
        /// business and is asserted at length elsewhere; what matters is that the GUI lands on top
        /// of it and leaves the rest of the picture exactly as the trace left it.
        TEST_F(RtxGuiDrawTest, theGuiLandsOverATracedFrameAndLeavesTheRestOfItAlone)
        {
            constexpr std::array<std::uint32_t, 6> sQuadIndices{ 0, 1, 2, 0, 2, 3 };
            const std::array<osg::Vec3f, 4> sWall{
                osg::Vec3f(-8000.0f, 200.0f, -8000.0f),
                osg::Vec3f(8000.0f, 200.0f, -8000.0f),
                osg::Vec3f(8000.0f, 200.0f, 8000.0f),
                osg::Vec3f(-8000.0f, 200.0f, 8000.0f),
            };

            SceneDesc scene;
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(sWall, {}, {}, sQuadIndices) });

            mRenderer->setScene(Rtx::sWorld, scene, {}, SeaState{});

            const Shaders::VisibilityConstants camera
                = makeCamera(osg::Vec3f(), osg::Vec3f(0.0f, 100.0f, 0.0f), 60.0f, sExtent, sExtent, 1000000.0f);

            mRenderer->renderFrame(camera, FrameOptions{});
            const std::array<std::uint8_t, 4> traced = at(sExtent - 2, 4);

            // The same camera and the same scene, so the same picture — and then the GUI over half
            // of it.
            mRenderer->renderFrame(camera, FrameOptions{});

            const std::uint32_t texture = makeTexel({ 17, 34, 51, 255 });
            drawQuad(texture, -1.0f, 1.0f, 0.0f, -1.0f, packColour(255, 255, 255, 255));

            EXPECT_EQ(at(1, 4), (std::array<std::uint8_t, 4>{ 17, 34, 51, 255 })) << "where the GUI drew";
            EXPECT_EQ(at(sExtent - 2, 4), traced) << "where it did not";
        }

        /// A level sheet of `extent` about the origin at z = 0, facing up.
        SceneDesc makeSheet(float extent)
        {
            constexpr std::array<std::uint32_t, 6> indices{ 0, 1, 2, 0, 2, 3 };
            const std::array<osg::Vec3f, 4> corners{
                osg::Vec3f(-extent, -extent, 0.0f),
                osg::Vec3f(extent, -extent, 0.0f),
                osg::Vec3f(extent, extent, 0.0f),
                osg::Vec3f(-extent, extent, 0.0f),
            };

            SceneDesc scene;
            scene.addInstance(MeshInstance{
                .mTransform = osg::Matrixf::identity(), .mMesh = scene.addMesh(corners, {}, {}, indices) });

            return scene;
        }

        /// Straight down at a sheet from a hundred units up, over a box two hundred across.
        Shaders::VisibilityConstants makeMapCamera(std::uint32_t extent)
        {
            Shaders::VisibilityConstants camera = makeOrthographicCameraFromView(
                osg::Matrixf::lookAt(osg::Vec3f(0.0f, 0.0f, 100.0f), osg::Vec3f(), osg::Vec3f(0.0f, 1.0f, 0.0f)),
                200.0f, 200.0f, extent, extent, 1.0f, 10000.0f);

            // Travelling straight down onto a sheet that faces up, so it is lit square on and the
            // picture is something rather than a coverage mask with nothing in it.
            camera.mSunPosition = osg::Vec3f(0.0f, 0.0f, 1.0f);
            camera.mSunIrradiance = osg::Vec3f(1.0f, 1.0f, 1.0f);

            return camera;
        }

        /// A picture traced into the table the GUI draws from, and where it stops.
        ///
        /// **The shape is the assertion and it is counted by hand.** The sheet is fifty units across
        /// inside a box two hundred across, so it covers a quarter of each axis: pixel `p` of sixteen
        /// samples the world at `100 * ((p + 0.5) / 8 - 1)`, which is inside twenty-five for `p` in
        /// 6..9. Four columns, four rows, and nothing on the boundary for rounding to argue over.
        ///
        /// What the two legs differ in is the one field that says so: with a sky behind it every
        /// pixel is opaque, and without one the picture says where it stops.
        TEST_F(RtxGuiDrawTest, aTracedPictureFillsAGuiTextureAndSaysWhereItStops)
        {
            constexpr std::uint32_t extent = 16;

            mRenderer->setScene(Rtx::sWorld, makeSheet(25.0f), {}, SeaState{});

            const std::uint32_t texture = mRenderer->addGuiTexture(extent, extent);
            mHeld.push_back(texture);

            Shaders::VisibilityConstants camera = makeMapCamera(extent);
            camera.mTransparentBackground = 1;

            mRenderer->traceGuiTexture(texture, camera, GuiTraceOptions{ .mWidth = extent, .mHeight = extent });

            for (std::uint32_t p : { 6u, 9u })
            {
                EXPECT_EQ(inTexture(texture, extent, p, 8)[3], 255) << "covered, column " << p;
                EXPECT_EQ(inTexture(texture, extent, 8, p)[3], 255) << "covered, row " << p;
            }

            for (std::uint32_t p : { 5u, 10u })
            {
                EXPECT_EQ(inTexture(texture, extent, p, 8), (std::array<std::uint8_t, 4>{ 0, 0, 0, 0 }))
                    << "past the sheet, column " << p;
                EXPECT_EQ(inTexture(texture, extent, 8, p), (std::array<std::uint8_t, 4>{ 0, 0, 0, 0 }))
                    << "past the sheet, row " << p;
            }

            // **Lit, and to the byte**, so the whole chain ran rather than only the coverage the
            // alpha above would have had either way. A default albedo of a half, Lambertian, square
            // to a sun of one: `0.5 * 1.0 / pi = 0.159155` linear, which the display curve encodes
            // as 111 of 255.
            EXPECT_EQ(inTexture(texture, extent, 8, 8), (std::array<std::uint8_t, 4>{ 111, 111, 111, 255 }))
                << "the sheet, lit";

            // The same picture with a sky behind it, which is what a frame filling a window has:
            // every pixel opaque, the corner included.
            //
            // **Twice, with nothing between.** A picture that fills its texture never clears it, so
            // this is the only path where the copy is the first thing to write it — and the second
            // of the two starts from a texture the first left where it found it rather than from
            // one nothing has touched. What it is really asking is whether the two are ordered at
            // all, which is a question only the synchronization layers answer.
            camera.mTransparentBackground = 0;
            for (int again = 0; again < 2; ++again)
                mRenderer->traceGuiTexture(texture, camera, GuiTraceOptions{ .mWidth = extent, .mHeight = extent });

            EXPECT_EQ(inTexture(texture, extent, 5, 8), (std::array<std::uint8_t, 4>{ 0, 0, 0, 255 }))
                << "past the sheet, and opaque";
        }

        /// A picture smaller than the texture behind it, which is the inventory doll: its window
        /// resizes and the texture does not.
        ///
        /// **The clear colour is exact.** It is written into an eight-bit unorm image, so one is
        /// 255 and not 254 — there is no encoding between the float and the byte.
        TEST_F(RtxGuiDrawTest, aPictureShorterThanItsTextureLeavesTheRestAtTheClearColour)
        {
            constexpr std::uint32_t extent = 16;
            constexpr std::uint32_t filled = 8;

            mRenderer->setScene(Rtx::sWorld, makeSheet(25.0f), {}, SeaState{});

            const std::uint32_t texture = mRenderer->addGuiTexture(extent, extent);
            mHeld.push_back(texture);

            Shaders::VisibilityConstants camera = makeMapCamera(filled);
            camera.mTransparentBackground = 1;

            mRenderer->traceGuiTexture(texture, camera,
                GuiTraceOptions{ .mWidth = filled, .mHeight = filled, .mClear = { 1.0f, 0.0f, 0.0f, 1.0f } });

            // The sheet now covers a quarter of eight pixels — `p` in 3..4 — so the middle of the
            // filled corner is on it and the corner past `filled` was never traced at all.
            EXPECT_EQ(inTexture(texture, extent, 4, 4)[3], 255) << "inside the picture";
            EXPECT_EQ(inTexture(texture, extent, filled, filled), (std::array<std::uint8_t, 4>{ 255, 0, 0, 255 }))
                << "just past it";
            EXPECT_EQ(
                inTexture(texture, extent, extent - 1, extent - 1), (std::array<std::uint8_t, 4>{ 255, 0, 0, 255 }))
                << "the far corner";
        }

        /// A picture of something that is not in the world at all: the inventory doll.
        ///
        /// **The two scenes are told apart by their shape and the count is exact.** The world holds
        /// a sheet that fills the box; the view scene holds one covering a quarter of each axis. Of
        /// sixteen pixels across, the first gives sixteen covered and the second four — `p` in 6..9,
        /// as above. Nothing about a doll may reach the world's geometry, and nothing about the
        /// frame may reach the doll's.
        TEST_F(RtxGuiDrawTest, aPictureCanBeOfASceneTheWorldDoesNotHold)
        {
            constexpr std::uint32_t extent = 16;

            // Two hundred across is the whole box, so the world's sheet covers every pixel.
            mRenderer->setScene(Rtx::sWorld, makeSheet(100.0f), {}, SeaState{});

            const std::uint32_t doll = mRenderer->addViewScene();
            mRenderer->setScene(doll, makeSheet(25.0f), {}, SeaState{});

            const std::uint32_t texture = mRenderer->addGuiTexture(extent, extent);
            mHeld.push_back(texture);

            Shaders::VisibilityConstants camera = makeMapCamera(extent);
            camera.mTransparentBackground = 1;

            const auto covered = [&](std::uint32_t scene) {
                mRenderer->traceGuiTexture(
                    texture, camera, GuiTraceOptions{ .mWidth = extent, .mHeight = extent, .mScene = scene });

                std::uint32_t across = 0;
                for (std::uint32_t x = 0; x < extent; ++x)
                    if (inTexture(texture, extent, x, 8)[3] != 0)
                        ++across;

                return across;
            };

            EXPECT_EQ(covered(sWorld), extent) << "the world fills the box";
            EXPECT_EQ(covered(doll), 4u) << "and the doll is a quarter of it";

            // The world is still the world afterwards: building one scene did not replace the other.
            EXPECT_EQ(covered(sWorld), extent) << "the world, still there";

            mRenderer->dropViewScene(doll);
        }

        /// The picture the trace made is the picture the GUI draws with, which is the whole point of
        /// it going into a slot rather than coming back to main memory.
        TEST_F(RtxGuiDrawTest, theGuiDrawsWithAPictureTheTraceMade)
        {
            constexpr std::uint32_t extent = 16;

            mRenderer->setScene(Rtx::sWorld, makeSheet(25.0f), {}, SeaState{});

            const std::uint32_t texture = mRenderer->addGuiTexture(extent, extent);
            mHeld.push_back(texture);

            Shaders::VisibilityConstants camera = makeMapCamera(extent);
            camera.mTransparentBackground = 1;
            mRenderer->traceGuiTexture(texture, camera, GuiTraceOptions{ .mWidth = extent, .mHeight = extent });

            // Blue underneath, then the traced picture over the whole frame. The middle samples the
            // sheet, which is opaque and covers the blue; the corner samples where the trace stopped,
            // which is transparent and leaves it.
            const std::uint32_t white = makeTexel({ 255, 255, 255, 255 });
            drawQuad(white, -1.0f, 1.0f, 1.0f, -1.0f, packColour(0, 0, 255, 255));
            drawQuad(texture, -1.0f, 1.0f, 1.0f, -1.0f, packColour(255, 255, 255, 255));

            EXPECT_NE(at(4, 4), (std::array<std::uint8_t, 4>{ 0, 0, 255, 255 })) << "where the picture covers";
            EXPECT_EQ(at(0, 0), (std::array<std::uint8_t, 4>{ 0, 0, 255, 255 })) << "where it does not";
        }

        /// Nothing to draw is not an error and does not touch the frame.
        TEST_F(RtxGuiDrawTest, anEmptyGuiLeavesTheFrameAlone)
        {
            const std::uint32_t white = makeTexel({ 255, 255, 255, 255 });
            drawQuad(white, -1.0f, 1.0f, 1.0f, -1.0f, packColour(17, 34, 51, 255));

            mRenderer->drawGui({}, {});

            EXPECT_EQ(at(4, 4), (std::array<std::uint8_t, 4>{ 17, 34, 51, 255 }));
        }
    }
}

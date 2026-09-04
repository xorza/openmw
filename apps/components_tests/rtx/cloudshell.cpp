#include <algorithm>
#include <cmath>
#include <cstddef>

#include <gtest/gtest.h>

#include <osg/Geometry>
#include <osg/Group>
#include <osg/Math>
#include <osg/MatrixTransform>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/cloudshell.hpp>
#include <components/rtx/shaders/look.h>
#include <components/rtx/shaders/scene.h>

namespace Rtx
{
    namespace
    {
        /// The layer the tests below are cut from: 300 up, falling by `0.0002 r²`, 400 units to a tile.
        ///
        /// At the outermost ring that is `300 - 0.0002 · 800² = 172`, which is about the proportion
        /// Morrowind's own cap has — it falls from 292 to 85 over a radius of 1000.
        constexpr float sHeight = 300.0f;
        constexpr float sCurvature = 0.0002f;
        constexpr float sTile = 400.0f;

        /// Morrowind's own cap, in Morrowind's own vertex order: four rings of sixteen out to `r = 800`,
        /// with the apex second.
        ///
        /// **The order is the test.** `ModVertexAlphaVisitor::Clouds` paints by index — 49 to 64 at
        /// nothing and 33 to 48 at a quarter — so a mesh laid out any other way is a mesh the engine
        /// fades somewhere else, and a reader that copies the rule has to be fed the layout the rule was
        /// written for. `sky_clouds_01.nif` puts one ring vertex at index 0, the apex at 1, and the rest
        /// of that ring from 2.
        ///
        /// **The sheet is laid the way Morrowind lays its own**: `u` running with the world's `x` and
        /// `v` against its `y`, which is the mirror that decides whether a deck drifts with its storm or
        /// into it.
        osg::ref_ptr<osg::Geometry> makeLayer(float height = sHeight, float curvature = sCurvature, float tile = sTile,
            float handedness = -1.0f, const osg::Vec2f& origin = osg::Vec2f(), int perRing = 16)
        {
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec2Array> coords = new osg::Vec2Array;

            const auto put = [&](float radius, int step) {
                const float turn = 2.0f * osg::PIf * float(step) / float(perRing);
                const osg::Vec2f at(radius * std::cos(turn), radius * std::sin(turn));

                vertices->push_back(osg::Vec3f(at.x(), at.y(), height - curvature * radius * radius));
                coords->push_back(origin + osg::Vec2f(at.x() / tile, handedness * at.y() / tile));
            };

            put(200.0f, 0);
            put(0.0f, 0);
            for (int step = 1; step < perRing; ++step)
                put(200.0f, step);

            for (int ring = 2; ring <= 4; ++ring)
                for (int step = 0; step < perRing; ++step)
                    put(200.0f * float(ring), step);

            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(vertices);
            geometry->setTexCoordArray(0, coords);
            return geometry;
        }

        /// How much deck the shader draws at a crossing radius. `cloudDeck` in `sky.glsl`.
        float fadesTo(const CloudShell& shell, float reach)
        {
            if (reach >= shell.mRings.z())
                return 0.0f;

            const auto ramp = [](float from, float to, float at, float over) {
                return from + (to - from) * (at / std::max(over, 1.0e-6f));
            };

            if (reach <= shell.mRings.x())
                return 1.0f;

            if (reach <= shell.mRings.y())
                return ramp(
                    1.0f, Shaders::CLOUD_RING_ALPHA, reach - shell.mRings.x(), shell.mRings.y() - shell.mRings.x());

            return ramp(Shaders::CLOUD_RING_ALPHA, 0.0f, reach - shell.mRings.y(), shell.mRings.z() - shell.mRings.y());
        }

        /// Where the shader puts a direction, in sheet coordinates. `cloudDeck` in `sky.glsl`, without
        /// the storm's turn and the scroll.
        osg::Vec2f landsAt(const CloudShell& shell, const osg::Vec3f& direction)
        {
            const osg::Vec2f plane(direction.x() / direction.z(), direction.y() / direction.z());
            const float fallen = 2.0f / (1.0f + std::sqrt(1.0f + 4.0f * shell.mCurvature * (plane * plane)));

            return osg::Vec2f(plane.x() * shell.mTiles.x(), plane.y() * shell.mTiles.y()) * fallen;
        }

        /// The two numbers are what the mesh says, and both of them are ratios.
        ///
        /// A height of 300 over a tile of 400 is 0.75 tiles, and a curvature of 0.0002 over that height
        /// is 0.06 — the sagitta's `k · h`, which is what makes it a pure number.
        TEST(RtxCloudShellTest, aLayerIsAHeightInTilesAndACurvature)
        {
            const CloudShell shell = readCloudShell(*makeLayer());

            EXPECT_NEAR(shell.mTiles.x(), 0.75f, 1.0e-4f);
            EXPECT_NEAR(shell.mTiles.y(), -0.75f, 1.0e-4f) << "the sheet was read the wrong way up";
            EXPECT_NEAR(shell.mCurvature, 0.06f, 1.0e-5f);

            // Laid the other way up, which is a sheet a replaced mesh may hand over.
            const CloudShell mirrored = readCloudShell(*makeLayer(sHeight, sCurvature, sTile, /*handedness=*/1.0f));
            EXPECT_NEAR(mirrored.mTiles.y(), 0.75f, 1.0e-4f);

            // And where in the sheet the mesh happened to start is not part of either number.
            const CloudShell shifted
                = readCloudShell(*makeLayer(sHeight, sCurvature, sTile, -1.0f, osg::Vec2f(2.5f, -1.5f)));
            EXPECT_NEAR(shifted.mTiles.x(), 0.75f, 1.0e-4f);
            EXPECT_NEAR(shifted.mCurvature, 0.06f, 1.0e-5f);
        }

        /// A ray lands where the mesh's own vertex does, which is the whole point of reading it.
        ///
        /// **The arithmetic in the shader and the mesh it stands for are two ways to one answer.** A
        /// vertex at `(x, y, z)` is both a direction the eye may look along and a point the sheet is
        /// pinned at, so the deck is right exactly when looking along it lands on the pin — and a flat
        /// layer, which is what the deck was drawn on before this, misses by more the lower it looks:
        /// a third at forty-five degrees and twice over at fifteen.
        TEST(RtxCloudShellTest, aRayLandsWhereTheMeshPinsTheSheet)
        {
            osg::ref_ptr<osg::Geometry> layer = makeLayer();
            const CloudShell shell = readCloudShell(*layer);

            const auto& vertices = *static_cast<const osg::Vec3Array*>(layer->getVertexArray());
            const auto& coords = *static_cast<const osg::Vec2Array*>(layer->getTexCoordArray(0));

            float furthest = 0.0f;
            for (std::size_t i = 0; i < vertices.size(); ++i)
            {
                osg::Vec3f direction = vertices[i];
                direction.normalize();

                const osg::Vec2f landed = landsAt(shell, direction);
                EXPECT_NEAR(landed.x(), coords[i].x(), 1.0e-3f) << "vertex " << i;
                EXPECT_NEAR(landed.y(), coords[i].y(), 1.0e-3f) << "vertex " << i;

                furthest = std::max(furthest, coords[i].length());
            }

            // The outermost ring is 800 units out on a 400-unit tile, and every ring is somewhere the
            // sheet actually reaches — so the loop above compared something rather than nothing.
            EXPECT_NEAR(furthest, 2.0f, 1.0e-3f);
        }

        /// The deck ends where the engine's own vertex alpha ends it.
        ///
        /// **`ModVertexAlphaVisitor::Clouds` reduced to three radii.** It paints the outermost ring of
        /// sixteen at nothing and the one inside it at `CLOUD_RING_ALPHA`, and a triangle between two
        /// rings interpolates that linearly in position — so the whole fade is where each band stops and
        /// a straight line between. Here the bands stop at 400, 600 and 800 units on a 400-unit tile,
        /// which is one tile, one and a half, and two.
        TEST(RtxCloudShellTest, theDeckEndsWhereTheEnginesOwnAlphaEndsIt)
        {
            const CloudShell shell = readCloudShell(*makeLayer());

            EXPECT_NEAR(shell.mRings.x(), 1.0f, 1.0e-4f);
            EXPECT_NEAR(shell.mRings.y(), 1.5f, 1.0e-4f);
            EXPECT_NEAR(shell.mRings.z(), 2.0f, 1.0e-4f);

            EXPECT_EQ(fadesTo(shell, 0.0f), 1.0f) << "straight up";
            EXPECT_EQ(fadesTo(shell, 1.0f), 1.0f) << "and out to the last whole ring";

            // Half way from one tile to one and a half is half way from a whole deck to a quarter of one.
            EXPECT_NEAR(fadesTo(shell, 1.25f), 0.5f * (1.0f + Shaders::CLOUD_RING_ALPHA), 1.0e-5f);
            EXPECT_NEAR(fadesTo(shell, 1.5f), Shaders::CLOUD_RING_ALPHA, 1.0e-5f);
            EXPECT_NEAR(fadesTo(shell, 1.75f), 0.5f * Shaders::CLOUD_RING_ALPHA, 1.0e-5f);

            EXPECT_EQ(fadesTo(shell, 2.0f), 0.0f) << "the rim";
            EXPECT_EQ(fadesTo(shell, 8.0f), 0.0f) << "and everything a levelling ray reaches past it";

            // **And the fade only ever falls**, which is what a mesh the rule makes nonsense of has to
            // come out as rather than a deck that thickens toward the horizon.
            float above = 1.0f;
            for (int step = 0; step <= 64; ++step)
            {
                const float here = fadesTo(shell, 2.5f * float(step) / 64.0f);
                EXPECT_LE(here, above + 1.0e-6f) << "at " << 2.5f * float(step) / 64.0f;
                above = here;
            }
        }

        /// A mesh the engine's rule does not reach paints every vertex whole, and the fade collapses.
        ///
        /// **Which is the same nonsense the rasterizer would draw.** The rule counts vertices and eight
        /// to a ring leaves 33 of them, so nothing lands in either faded band — the deck then runs to its
        /// own rim at full strength and stops, because a band nobody painted reaches no further than the
        /// one inside it.
        TEST(RtxCloudShellTest, aMeshTheRuleDoesNotReachIsDeckToItsRim)
        {
            const CloudShell shell
                = readCloudShell(*makeLayer(sHeight, sCurvature, sTile, -1.0f, osg::Vec2f(), /*perRing=*/8));
            EXPECT_NEAR(shell.mRings.x(), 2.0f, 1.0e-4f);
            EXPECT_EQ(shell.mRings.x(), shell.mRings.y());
            EXPECT_EQ(shell.mRings.y(), shell.mRings.z());

            EXPECT_EQ(fadesTo(shell, 1.9f), 1.0f);
            EXPECT_EQ(fadesTo(shell, 2.0f), 0.0f);
        }

        /// What the graph does to a mesh is part of where it hangs.
        ///
        /// **Morrowind's cap needs this**: its `NiTriShape` sits fifteen units under its `NiNode`, which
        /// is a twentieth of the height every number here is a ratio against. And a uniform scale is not,
        /// because both numbers are ratios — a layer modelled twice as large is the same sky.
        TEST(RtxCloudShellTest, theTransformOverAMeshIsPartOfWhereItHangs)
        {
            osg::ref_ptr<osg::MatrixTransform> lowered = new osg::MatrixTransform;
            lowered->setMatrix(osg::Matrix::translate(0.0f, 0.0f, -15.0f));
            lowered->addChild(makeLayer());

            // 285 over the same 400-unit tile, and the curvature it stands against comes down with it.
            const CloudShell dropped = readCloudShell(*lowered);
            EXPECT_NEAR(dropped.mTiles.x(), 0.7125f, 1.0e-4f);
            EXPECT_NEAR(dropped.mCurvature, 0.057f, 1.0e-5f);

            osg::ref_ptr<osg::MatrixTransform> enlarged = new osg::MatrixTransform;
            enlarged->setMatrix(osg::Matrix::scale(2.0f, 2.0f, 2.0f));
            enlarged->addChild(makeLayer());

            const CloudShell doubled = readCloudShell(*enlarged);
            EXPECT_NEAR(doubled.mTiles.x(), 0.75f, 1.0e-4f) << "the scale the mesh was modelled at got in";
            EXPECT_NEAR(doubled.mCurvature, 0.06f, 1.0e-5f);
        }

        /// A layer that does not curve is the plane the deck used to be drawn on, and that still works.
        TEST(RtxCloudShellTest, aFlatSheetIsTheSameLayerWithNoCurve)
        {
            const CloudShell flat = readCloudShell(*makeLayer(sHeight, /*curvature=*/0.0f));

            EXPECT_NEAR(flat.mTiles.x(), 0.75f, 1.0e-4f);
            EXPECT_EQ(flat.mCurvature, 0.0f);

            // Straight up it makes no difference at all, and lower down it is the whole difference. At
            // forty-five degrees the plane lands at 0.75 and the layer at `0.75 · 2 / (1 + sqrt(1.24))`,
            // which is 0.7097; by fifteen degrees, where `t` is 3.732, the two are 2.799 against 1.815.
            const osg::Vec3f slanted(std::sqrt(0.5f), 0.0f, std::sqrt(0.5f));
            EXPECT_NEAR(landsAt(flat, slanted).x(), 0.75f, 1.0e-4f);
            EXPECT_NEAR(landsAt(readCloudShell(*makeLayer()), slanted).x(), 0.70970f, 1.0e-4f);

            // A mesh that rises as it recedes is not a layer over anything, and is taken as the flat one.
            const CloudShell domed = readCloudShell(*makeLayer(sHeight, /*curvature=*/-0.0002f));
            EXPECT_EQ(domed.mCurvature, 0.0f);
        }

        /// A mesh with nothing in it hangs nothing, rather than dividing by what it did not say.
        TEST(RtxCloudShellTest, aMeshThatSaysNothingHangsNoLayer)
        {
            EXPECT_EQ(readCloudShell(*new osg::Group).mTiles, osg::Vec2f());

            osg::ref_ptr<osg::Geometry> bare = new osg::Geometry;
            bare->setVertexArray(new osg::Vec3Array);
            bare->setTexCoordArray(0, new osg::Vec2Array);
            EXPECT_EQ(readCloudShell(*bare).mTiles, osg::Vec2f());

            // One ring at one radius: every vertex agrees about the height, so there is no curvature to
            // read and the fit would divide by their agreement.
            osg::ref_ptr<osg::Vec3Array> ring = new osg::Vec3Array;
            osg::ref_ptr<osg::Vec2Array> coords = new osg::Vec2Array;
            for (int step = 0; step < 8; ++step)
            {
                const float turn = 0.25f * osg::PIf * float(step);
                ring->push_back(osg::Vec3f(500.0f * std::cos(turn), 500.0f * std::sin(turn), 100.0f));
                coords->push_back(osg::Vec2f(ring->back().x() / sTile, -ring->back().y() / sTile));
            }

            osg::ref_ptr<osg::Geometry> flat = new osg::Geometry;
            flat->setVertexArray(ring);
            flat->setTexCoordArray(0, coords);
            EXPECT_EQ(readCloudShell(*flat).mTiles, osg::Vec2f());
        }
    }
}

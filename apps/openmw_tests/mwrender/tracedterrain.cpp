#include <optional>

#include <gtest/gtest.h>

#include <osg/Group>
#include <osg/Vec3d>
#include <osg/Vec3f>
#include <osg/ref_ptr>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>

#include <apps/components_tests/rtx/support/fakeland.hpp>
#include <apps/openmw/mwrender/rtx/tracedterrain.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/terrain/view.hpp>

namespace MWRender
{
    namespace
    {
        constexpr unsigned int sTerrainMask = 1u << 8;

        /// Where a ray straight down over (`x`, `y`) meets what stands under `root` with
        /// `sTerrainMask`, or nothing.
        std::optional<osg::Vec3d> groundUnder(osg::Group& root, const double x, const double y)
        {
            osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector = new osgUtil::LineSegmentIntersector(
                osgUtil::LineSegmentIntersector::MODEL, osg::Vec3d(x, y, 10000.0), osg::Vec3d(x, y, -10000.0));
            osgUtil::IntersectionVisitor visitor(intersector);
            visitor.setTraversalMask(sTerrainMask);
            root.accept(visitor);

            if (!intersector->containsIntersections())
                return std::nullopt;

            return intersector->getFirstIntersection().getWorldIntersectPoint();
        }

        /// **The ground answers upstream's callers as a world with no chunks.** The preloader asks
        /// for a view and resets it on a worker thread; `tb` toggles borders and reports what it
        /// got. Both used to reach a null and a guard in an upstream file; both now get an answer.
        TEST(RtxTracedTerrainTest, aViewIsHandedOutAndBordersStayOff)
        {
            Rtx::Testing::FakeLand land;
            osg::ref_ptr<osg::Group> sceneRoot = new osg::Group;
            TracedTerrain ground(*sceneRoot, land, sTerrainMask, ESM::Cell::sDefaultWorldspaceId);

            EXPECT_EQ(sceneRoot->getNumChildren(), 1u) << "the terrain root the game masks and finds";

            const osg::ref_ptr<Terrain::View> view = ground.createView();
            ASSERT_NE(view, nullptr);
            view->reset();

            ground.setBordersVisible(true);
            EXPECT_FALSE(ground.getBordersVisible());
        }

        /// A loaded cell stands ground for the intersector at the storage's own height, and an
        /// unloaded one stands none.
        ///
        /// **`RenderingManager::castRay` walks the scene graph under `Mask_Terrain`**, and the
        /// ring's ground is on the device where no `osgUtil` visitor reaches. The fake land is a
        /// plane of eight units a column and sixteen a row, so its middle vertex, thirty-two of
        /// each, stands at 768 — and a vertex, so the answer is exact rather than interpolated.
        /// The cell is loaded twice with an unload between, because the second load stands on the
        /// grid the first gave back.
        TEST(RtxTracedTerrainTest, aLoadedCellAnswersADownwardRayAtTheLandsHeight)
        {
            Rtx::Testing::FakeLand land;
            land.mWithData.push_back(osg::Vec2i(0, 0));

            osg::ref_ptr<osg::Group> sceneRoot = new osg::Group;
            TracedTerrain ground(*sceneRoot, land, sTerrainMask, ESM::Cell::sDefaultWorldspaceId);

            constexpr double cell = static_cast<double>(Rtx::Testing::FakeLand::sCellSize);
            constexpr double middle = 0.5 * cell;
            EXPECT_FALSE(groundUnder(*sceneRoot, middle, middle).has_value()) << "ground before any cell was loaded";

            for (int again = 0; again < 2; ++again)
            {
                ground.loadCell(0, 0);
                const std::optional<osg::Vec3d> met = groundUnder(*sceneRoot, middle, middle);
                ASSERT_TRUE(met.has_value()) << "the loaded cell stood no ground, on load " << again;
                EXPECT_NEAR(met->z(), static_cast<double>(Rtx::Testing::FakeLand::heightAt(32, 32)), 1e-3);
                EXPECT_NEAR(met->x(), middle, 1e-3);
                EXPECT_NEAR(met->y(), middle, 1e-3);

                // A quarter of the way across, which is not a vertex: column and row sixteen
                // stand at 384, and the diamond's triangles are planes through the vertices, so
                // the plane's own value is the answer wherever a ray lands on it.
                constexpr double quarter = 0.25 * cell;
                const std::optional<osg::Vec3d> slope = groundUnder(*sceneRoot, quarter + 10.0, quarter + 30.0);
                ASSERT_TRUE(slope.has_value());
                EXPECT_NEAR(slope->z(), 8.0 * (16.0 + 10.0 / 128.0) + 16.0 * (16.0 + 30.0 / 128.0), 1e-2);

                ground.unloadCell(0, 0);
                EXPECT_FALSE(groundUnder(*sceneRoot, middle, middle).has_value()) << "ground after the cell left";
            }

            // The other cell of the same worldspace is not the one that was loaded.
            ground.loadCell(0, 0);
            EXPECT_FALSE(groundUnder(*sceneRoot, middle + cell, middle).has_value());
        }
    }
}

#include <gtest/gtest.h>

#include <osg/Group>
#include <osg/ref_ptr>

#include <components/esm3/loadcell.hpp>
#include <components/terrain/view.hpp>

#include "apps/components_tests/rtx/fakeland.hpp"
#include "apps/openmw/mwrender/rtx/tracedground.hpp"

namespace MWRender
{
    namespace
    {
        /// **The ground answers upstream's callers as a world with no chunks.** The preloader asks
        /// for a view and resets it on a worker thread; `tb` toggles borders and reports what it
        /// got. Both used to reach a null and a guard in an upstream file; both now get an answer.
        TEST(RtxTracedGroundTest, aViewIsHandedOutAndBordersStayOff)
        {
            Rtx::Testing::FakeLand land;
            osg::ref_ptr<osg::Group> sceneRoot = new osg::Group;
            TracedGround ground(*sceneRoot, land, 1u, ESM::Cell::sDefaultWorldspaceId);

            EXPECT_EQ(sceneRoot->getNumChildren(), 1u) << "the terrain root the game masks and finds";

            const osg::ref_ptr<Terrain::View> view = ground.createView();
            ASSERT_NE(view, nullptr);
            view->reset();

            ground.setBordersVisible(true);
            EXPECT_FALSE(ground.getBordersVisible());

            ground.loadCell(0, 0);
            ground.unloadCell(0, 0);
        }
    }
}

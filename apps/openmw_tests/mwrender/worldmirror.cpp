#include <gtest/gtest.h>

#include <osg/Node>

#include <components/sceneutil/vismask.hpp>

#include "apps/openmw/mwrender/rtx/worldmirror.hpp"

namespace MWRender
{
    namespace
    {
        /// **The player is the one thing a mirror leaves out on a question about the camera.** Every
        /// other exclusion is a fact about the subtree — the sky is drawn by the trace, the simple
        /// water is a duplicate — and this one is a fact about who is looking. A camera standing
        /// where the player stands traced a boot thirteen units from the eye.
        TEST(RtxWorldMirrorTest, thePlayerIsWalkedOnlyForACameraThatIsTheirEye)
        {
            WorldMirror mirror(MirrorSettings{});

            const osg::Node::NodeMask playing = mirror.getExtractor().getTraversalMask();
            EXPECT_NE(playing & SceneUtil::Mask_Player, 0u) << "a game somebody is playing draws them";

            mirror.setShowsPlayer(false);
            const osg::Node::NodeMask watching = mirror.getExtractor().getTraversalMask();
            EXPECT_EQ(watching & SceneUtil::Mask_Player, 0u);

            // **And nothing else moved with it.** The mask carries the sky, the sun, the duplicate
            // water and whatever the content hid, and a recompute that dropped one of those would
            // trace a world with no ground or draw the sea twice.
            EXPECT_EQ(watching, playing & ~static_cast<osg::Node::NodeMask>(SceneUtil::Mask_Player));

            mirror.setShowsPlayer(true);
            EXPECT_EQ(mirror.getExtractor().getTraversalMask(), playing) << "and it comes back";
        }
    }
}

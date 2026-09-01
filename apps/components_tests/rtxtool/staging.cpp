#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <components/rtx/scenedesc.hpp>

#include <apps/rtxtool/content.hpp>
#include <apps/rtxtool/posedactors.hpp>
#include <apps/rtxtool/stagedworld.hpp>
#include <apps/rtxtool/world.hpp>

#include "installation.hpp"

namespace RtxTool
{
    namespace
    {
        /// A lamp-lit interior with a flame in it, which is what makes the lights worth comparing.
        constexpr std::string_view sRoom = "Seyda Neen, Census and Excise Office";

        struct RtxStagingTest : InstallationTest
        {
        };

        /// Staging one cell twice in one process describes the same world both times.
        ///
        /// **The whole class of defect this catches lives between the two stagings and nowhere
        /// else.** A staged world is supposed to be a function of the cell, the request and the
        /// clock — and it was not, three times over: `osgParticle` draws every raindrop's place from
        /// the C library's `std::rand`, a `SceneUtil::LightSource` takes its id from a counter that
        /// runs for the whole process, and `Rtx::lightPhase` turns that id into where a flame stands
        /// in its cycle. Each one made a cell rendered second look unlike the same cell rendered
        /// first, and each was found by a `verify` run disagreeing with itself rather than here.
        /// `StagedWorld::seedDraws` and the light-id reset beside its first call are what answer
        /// them.
        ///
        /// **The lights and the sprites, because those are what carried it.** The geometry never
        /// moved — the instance count, the texture count and the share of primary rays that hit were
        /// identical throughout — so a comparison of those would have passed while half of a
        /// lamp-lit room was a different brightness.
        ///
        /// **One at a time, because a staged world gives its ground back when it goes.** Two of them
        /// alive at once would be two worlds sharing one `World`, which is a case nothing else in
        /// this harness makes.
        TEST_F(RtxStagingTest, oneCellStagedTwiceDescribesTheSameWorld)
        {
            const ESM::Cell* room = getContent().findCell(std::string(sRoom));
            ASSERT_NE(room, nullptr);

            const StagingRequest request;
            const ActorRequest actors;

            std::vector<Rtx::Light> lights;
            std::vector<Rtx::Sprite> sprites;
            std::size_t placed = 0;

            {
                StagedWorld first(getWorld(), *room, request, actors);
                ASSERT_FALSE(first.empty());

                const Rtx::SceneDesc& scene = first.getScene();
                lights.assign(scene.getLights().begin(), scene.getLights().end());
                sprites.assign(scene.getSprites().begin(), scene.getSprites().end());
                placed = scene.getPlacedCount();
            }

            ASSERT_FALSE(lights.empty()) << "the room lit itself with nothing, so this proves nothing";
            ASSERT_FALSE(sprites.empty()) << "the room held no particle, so this proves half of nothing";

            {
                StagedWorld second(getWorld(), *room, request, actors);
                ASSERT_FALSE(second.empty());

                const Rtx::SceneDesc& scene = second.getScene();
                EXPECT_EQ(scene.getPlacedCount(), placed);

                ASSERT_EQ(scene.getLights().size(), lights.size());
                for (std::size_t at = 0; at < lights.size(); ++at)
                {
                    EXPECT_EQ(scene.getLights()[at].mPosition, lights[at].mPosition) << "light " << at;

                    // **The flicker rides here**, as the recorded colour times where the flame
                    // stands in its cycle. A phase drawn from a process counter moves this and
                    // nothing else in the record.
                    EXPECT_EQ(scene.getLights()[at].mIntensity, lights[at].mIntensity) << "light " << at;
                    EXPECT_EQ(scene.getLights()[at].mReach, lights[at].mReach) << "light " << at;
                }

                ASSERT_EQ(scene.getSprites().size(), sprites.size());
                for (std::size_t at = 0; at < sprites.size(); ++at)
                {
                    EXPECT_EQ(scene.getSprites()[at].mPosition, sprites[at].mPosition) << "sprite " << at;
                    EXPECT_EQ(scene.getSprites()[at].mAlpha, sprites[at].mAlpha) << "sprite " << at;
                }
            }
        }
    }
}

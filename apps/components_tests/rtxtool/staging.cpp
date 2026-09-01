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
        /// **The whole description and not the half that carried it.** Every one of the three moved
        /// the lights or the sprites and left the geometry exactly where it was, so a test of the
        /// instances alone would have passed while half of a lamp-lit room was a different
        /// brightness. What the title claims is that the world is the same, and the cheapest way to
        /// keep that claim true of a fourth member nobody has met is to compare all of it.
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

            std::vector<Rtx::MeshInstance> instances;
            std::vector<VFS::Path::Normalized> textures;
            std::vector<Rtx::Light> lights;
            std::vector<Rtx::Sprite> sprites;
            std::size_t meshes = 0;
            std::size_t materials = 0;
            std::size_t placed = 0;

            {
                StagedWorld first(getWorld(), *room, request, actors);
                ASSERT_FALSE(first.empty());

                const Rtx::SceneDesc& scene = first.getScene();
                instances.assign(scene.getInstances().begin(), scene.getInstances().end());
                textures.assign(scene.getTextures().begin(), scene.getTextures().end());
                lights.assign(scene.getLights().begin(), scene.getLights().end());
                sprites.assign(scene.getSprites().begin(), scene.getSprites().end());
                meshes = scene.getMeshes().size();
                materials = scene.getMaterials().size();
                placed = scene.getPlacedCount();
            }

            ASSERT_FALSE(lights.empty()) << "the room lit itself with nothing, so this proves nothing";
            ASSERT_FALSE(sprites.empty()) << "the room held no particle, so this proves half of nothing";

            {
                StagedWorld second(getWorld(), *room, request, actors);
                ASSERT_FALSE(second.empty());

                const Rtx::SceneDesc& scene = second.getScene();
                EXPECT_EQ(scene.getPlacedCount(), placed);
                EXPECT_EQ(scene.getMeshes().size(), meshes);
                EXPECT_EQ(scene.getMaterials().size(), materials);

                ASSERT_EQ(scene.getInstances().size(), instances.size());
                for (std::size_t at = 0; at < instances.size(); ++at)
                {
                    EXPECT_EQ(scene.getInstances()[at].mMesh, instances[at].mMesh) << "instance " << at;
                    EXPECT_EQ(scene.getInstances()[at].mMaterial, instances[at].mMaterial) << "instance " << at;
                    EXPECT_EQ(scene.getInstances()[at].mTransform, instances[at].mTransform) << "instance " << at;
                }

                ASSERT_EQ(scene.getTextures().size(), textures.size());
                for (std::size_t at = 0; at < textures.size(); ++at)
                    EXPECT_EQ(scene.getTextures()[at], textures[at]) << "texture " << at;

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

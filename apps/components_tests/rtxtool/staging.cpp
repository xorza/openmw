#include <cstddef>
#include <memory>
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

        /// Seyda Neen's own cell, and the nearest other place the corpus stands a camera.
        ///
        /// **Near enough that their distant reaches overlap, which is the whole fixture.** The cells
        /// `DistantLights` reads around one of these are read around the other too, so staging
        /// Balmora is what leaves the shore's own lights already built when the shore is staged
        /// after it. Ald-ruhn, Sadrith Mora and Dagon Fel are all too far to share a cell, and none
        /// of them showed the defect this covers.
        constexpr std::string_view sShore = "-2,-9";
        constexpr std::string_view sTown = "-3,-2";

        struct RtxStagingTest : InstallationTest
        {
        };

        /// What a staged world came to, held past the world that described it.
        struct Description
        {
            std::vector<Rtx::MeshInstance> mInstances;
            std::vector<VFS::Path::Normalized> mTextures;
            std::vector<Rtx::Light> mLights;
            std::vector<Rtx::Sprite> mSprites;
            std::size_t mMeshes = 0;
            std::size_t mMaterials = 0;
            std::size_t mPlaced = 0;
        };

        Description describe(const Rtx::SceneDesc& scene)
        {
            return Description{
                .mInstances = { scene.getInstances().begin(), scene.getInstances().end() },
                .mTextures = { scene.getTextures().begin(), scene.getTextures().end() },
                .mLights = { scene.getLights().begin(), scene.getLights().end() },
                .mSprites = { scene.getSprites().begin(), scene.getSprites().end() },
                .mMeshes = scene.getMeshes().size(),
                .mMaterials = scene.getMaterials().size(),
                .mPlaced = scene.getPlacedCount(),
            };
        }

        /// Every light, in the order the walk met them.
        ///
        /// **Its own function because an exterior can claim this and no more.** Two `World`s number
        /// one cell's meshes, materials and textures differently, because `SceneUtil::Optimizer`
        /// orders a model's drawables by pointer and two `Resource::SceneManager`s in one process
        /// hand out different addresses — `OPENMW_OPTIMIZE=OFF` makes three worlds byte-identical
        /// and is how that was pinned down. Each world is consistent with itself and draws the same
        /// picture, so the tables are a permutation and not a fault. A comparison across two of them
        /// has to say what it is comparing, and the lights are keyed on nothing the optimizer
        /// touches.
        void expectSameLights(const Rtx::SceneDesc& scene, const Description& was)
        {
            ASSERT_EQ(scene.getLights().size(), was.mLights.size());
            for (std::size_t at = 0; at < was.mLights.size(); ++at)
            {
                EXPECT_EQ(scene.getLights()[at].mPosition, was.mLights[at].mPosition) << "light " << at;

                // **The flicker rides here**, as the recorded colour times where the flame stands in
                // its cycle. A phase drawn from a process counter moves this and nothing else in the
                // record.
                EXPECT_EQ(scene.getLights()[at].mIntensity, was.mLights[at].mIntensity) << "light " << at;
                EXPECT_EQ(scene.getLights()[at].mReach, was.mLights[at].mReach) << "light " << at;
            }
        }

        /// Everything one staged world came to, against everything another did.
        ///
        /// **The whole description and not the half that carried it.** Every defect this file covers
        /// moved the lights or the sprites and left the geometry exactly where it was, so a
        /// comparison of the instances alone would have passed while half of a lamp-lit room was a
        /// different brightness.
        void expectSame(const Rtx::SceneDesc& scene, const Description& was)
        {
            expectSameLights(scene, was);

            EXPECT_EQ(scene.getPlacedCount(), was.mPlaced);
            EXPECT_EQ(scene.getMeshes().size(), was.mMeshes);
            EXPECT_EQ(scene.getMaterials().size(), was.mMaterials);

            ASSERT_EQ(scene.getInstances().size(), was.mInstances.size());
            for (std::size_t at = 0; at < was.mInstances.size(); ++at)
            {
                EXPECT_EQ(scene.getInstances()[at].mMesh, was.mInstances[at].mMesh) << "instance " << at;
                EXPECT_EQ(scene.getInstances()[at].mMaterial, was.mInstances[at].mMaterial) << "instance " << at;
                EXPECT_EQ(scene.getInstances()[at].mTransform, was.mInstances[at].mTransform) << "instance " << at;
            }

            ASSERT_EQ(scene.getTextures().size(), was.mTextures.size());
            for (std::size_t at = 0; at < was.mTextures.size(); ++at)
                EXPECT_EQ(scene.getTextures()[at], was.mTextures[at]) << "texture " << at;

            ASSERT_EQ(scene.getSprites().size(), was.mSprites.size());
            for (std::size_t at = 0; at < was.mSprites.size(); ++at)
            {
                EXPECT_EQ(scene.getSprites()[at].mPosition, was.mSprites[at].mPosition) << "sprite " << at;
                EXPECT_EQ(scene.getSprites()[at].mAlpha, was.mSprites[at].mAlpha) << "sprite " << at;
            }
        }

        /// Staging one cell twice in one process describes the same world both times.
        ///
        /// **The whole class of defect this catches lives between the two stagings and nowhere
        /// else.** A staged world is supposed to be a function of the cell, the request and the
        /// clock — and it was not, three times over: `osgParticle` draws every raindrop's place from
        /// the C library's `std::rand`, a `SceneUtil::LightSource` takes its id from a counter that
        /// runs for the whole process, and `Rtx::lightPhase` turns that id into where a flame stands
        /// in its cycle. Each one made a cell rendered second look unlike the same cell rendered
        /// first, and each was found by a `verify` run disagreeing with itself rather than here.
        /// `StagedWorld::seedDraws` and `World::beginStaging` beside its first call are what answer
        /// them.
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

            Description first;
            {
                StagedWorld staged(getWorld(), *room, request, actors);
                ASSERT_FALSE(staged.empty());
                first = describe(staged.getScene());
            }

            ASSERT_FALSE(first.mLights.empty()) << "the room lit itself with nothing, so this proves nothing";
            ASSERT_FALSE(first.mSprites.empty()) << "the room held no particle, so this proves half of nothing";

            {
                StagedWorld staged(getWorld(), *room, request, actors);
                ASSERT_FALSE(staged.empty());
                expectSame(staged.getScene(), first);
            }
        }

        /// A distant flame stands where it stands whatever was staged before it.
        ///
        /// **The counterpart above cannot reach this, and the cache is why.** `DistantLights` reads a
        /// cell once and keeps it, so a world that has read the shore's cells hands the same ones
        /// over on every later staging and agrees with itself while the defect is there. What the
        /// defect moves is *which* staging built them: read while another exterior was being staged,
        /// they carry ids out of that staging's light-id sequence rather than the shore's own — and
        /// `Rtx::lightPhase` reads the id for where a flame stands in its cycle. A cached campfire
        /// therefore burnt at a phase from a sequence that no longer exists, and could hold an id
        /// this staging had since handed to a lamp in the cell, which is two flames swinging as one.
        ///
        /// **Two worlds, because that is where the order lives.** One stages the shore alone, which
        /// is what `verify --views=seyda-neen-shore` does; the other stages Balmora and then the
        /// shore, which is what the same tool does over a list. Measured before
        /// `World::beginStaging` dropped the cache, the two disagreed on 46% of the shore's pixels.
        ///
        /// **The lights and not the whole description**, for the reason `expectSameLights` gives.
        TEST_F(RtxStagingTest, aDistantFlameStandsWhereItStandsWhateverWasStagedBeforeIt)
        {
            const ESM::Cell* shore = getContent().findCell(sShore);
            const ESM::Cell* town = getContent().findCell(sTown);
            ASSERT_NE(shore, nullptr);
            ASSERT_NE(town, nullptr);

            const StagingRequest request;
            const ActorRequest actors;

            // **Paged, because a gridded world stands no distant light at all.** `DistantLights` is
            // built beside the quad tree and handed over only where there is one.
            const auto pagedWorld = [this] {
                std::unique_ptr<World> made = openWorld();
                made->pageTerrain(true);

                return made;
            };

            Description alone;
            {
                const std::unique_ptr<World> world = pagedWorld();
                StagedWorld staged(*world, *shore, request, actors);
                ASSERT_FALSE(staged.empty());
                alone = describe(staged.getScene());
            }

            // The shore's own cell holds 69 lights and the reach around it another 405, so a count in
            // the hundreds is what says the distant ones arrived and the fixture means something.
            ASSERT_GT(alone.mLights.size(), 100u) << "no distant light was stood, so this proves nothing";

            {
                const std::unique_ptr<World> world = pagedWorld();

                {
                    StagedWorld staged(*world, *town, request, actors);
                    ASSERT_FALSE(staged.empty());
                }

                StagedWorld staged(*world, *shore, request, actors);
                ASSERT_FALSE(staged.empty());
                expectSameLights(staged.getScene(), alone);
            }
        }
    }
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec3f>

#include <apps/rtxtool/content.hpp>
#include <apps/rtxtool/motion.hpp>
#include <apps/rtxtool/posedactors.hpp>
#include <apps/rtxtool/stagedworld.hpp>
#include <apps/rtxtool/world.hpp>
#include <components/rtx/scenedesc.hpp>

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

        /// What a drop reports having travelled is its own fall and not the eye's walk.
        ///
        /// **The box of rain hangs from the eye and every step of that eye is taken back out of the
        /// drops again** — `Weather::WrapAroundOperator` slides each particle back by what the eye
        /// moved, which is what keeps a handful of drops around the camera and leaves them standing
        /// in the world. So a drop's travel between two frames is its fall, whatever the player
        /// did, and that is what the reprojection is handed. Adding the carriage back on — which is
        /// right for a torch on a walking arm and wrong for this — reads as rain that streaks at one
        /// heading and stands still at another.
        ///
        /// **Both of the boxes the weather builds, because they were built in two orders.** The
        /// rain node ran its wrap before its integration and the snow node ran it after, and a
        /// particle records where it was as it integrates — so the snow's previous position was in
        /// the box before the slide and its current one in the box after, and the eye's whole step
        /// came out in the difference. Rain passed this and snow failed it.
        ///
        /// The median over the drops, because they all fall at one speed and a handful of them wrap
        /// to the far side of the box on any frame the eye moves.
        TEST_F(RtxStagingTest, whatADropReportsHavingTravelledIsItsOwnFall)
        {
            const ESM::Cell* shore = getContent().findCell(std::string(sShore));
            ASSERT_NE(shore, nullptr);

            const osg::Vec3f over(-8292.0f, -73376.0f, 200.0f);

            const ActorRequest actors{ .mResidents = false, .mProps = true };

            for (const std::string_view weather : { "Rain", "Snow" })
            {
                StagingRequest request;
                request.mWeather = std::string(weather);
                request.mOrigin = over;
                request.mTarget = over + osg::Vec3f(0.0f, 1000.0f, 0.0f);

                /// The middle of what the drops say they travelled, after a run of frames that ends
                /// with the eye taking `step`.
                const auto travelled = [&](const osg::Vec3f& step) {
                    StagedWorld staged(getWorld(), *shore, request, actors);
                    Motion* motion = staged.getMotion();
                    EXPECT_NE(motion, nullptr);

                    for (std::uint32_t frame = 1; motion != nullptr && frame <= 30; ++frame)
                    {
                        staged.moveTo(frame < 30 ? over : over + step);
                        motion->step(frame);
                    }

                    // Each axis on its own, because the median of a length would hide a sideways
                    // step under a fall ten times its size.
                    osg::Vec3f middle;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        std::vector<float> along;
                        for (const Rtx::Sprite& sprite : staged.getScene().getSprites())
                            along.push_back(sprite.mMoved[axis]);

                        std::sort(along.begin(), along.end());
                        middle[axis] = along.empty() ? 0.0f : along[along.size() / 2];
                    }

                    return middle;
                };

                // One at a time, for the reason the first test in this file gives.
                const osg::Vec3f still = travelled(osg::Vec3f());
                ASSERT_LT(still.z(), -0.1f) << weather << ": nothing fell, so this proves nothing";

                // A step the size of a sprint, across the drops rather than along them.
                const osg::Vec3f walking = travelled(osg::Vec3f(60.0f, 0.0f, 0.0f));

                EXPECT_NEAR(walking.x(), still.x(), 0.5f)
                    << weather << ": the eye's walk came back in what the drops said they did";
                EXPECT_NEAR(walking.z(), still.z(), 0.05f * std::abs(still.z())) << weather;
            }
        }

        /// Nothing falls where the eye is under water.
        ///
        /// **Frozen is not gone, and the harness was reading the frozen one.**
        /// `Weather::Precipitation` stops the rain where it stands when the eye goes under and
        /// leaves what to draw to whoever is drawing — so a walk that kept meeting the subtree
        /// placed the drops the surface was crossed with, and they hung in the air for as long as
        /// the eye stayed down there. The rasterizer answers this by not culling the subtree and
        /// `MWRender::WorldMirror` by not walking it.
        ///
        /// **A run of frames and not a still**, because a still under water has no rain to leave
        /// behind: the system is frozen before it has emitted anything, and the defect is what it
        /// does with what is already falling.
        ///
        /// The same shore, the same frames and the same props either way, so what the two counts
        /// differ by is the weather and nothing else.
        TEST_F(RtxStagingTest, nothingFallsWhereTheEyeIsUnderWater)
        {
            const ESM::Cell* shore = getContent().findCell(std::string(sShore));
            ASSERT_NE(shore, nullptr);

            // Over the water off Seyda Neen's docks, and under it: the sea there stands at nought.
            const osg::Vec3f over(-8292.0f, -73376.0f, 200.0f);
            const osg::Vec3f under(-8292.0f, -73376.0f, -200.0f);

            // The props, because they are what puts a `Motion` in the world at all — and they run
            // in both halves of this, which is why the two are compared rather than either read on
            // its own.
            const ActorRequest actors{ .mResidents = false, .mProps = true };

            constexpr std::uint32_t sFrames = 30;

            /// The sprites standing after a run of frames with the eye wherever `stand` puts it.
            const auto sprited = [&](std::string_view weather, const osg::Vec3f& first, const osg::Vec3f& then) {
                StagingRequest request;
                request.mWeather = weather;
                request.mOrigin = first;
                request.mTarget = first + osg::Vec3f(0.0f, 1000.0f, 0.0f);

                StagedWorld staged(getWorld(), *shore, request, actors);
                Motion* motion = staged.getMotion();
                EXPECT_NE(motion, nullptr) << "the props were meant to make this world move";

                for (std::uint32_t frame = 1; motion != nullptr && frame <= 2 * sFrames; ++frame)
                {
                    staged.moveTo(frame <= sFrames ? first : then);
                    motion->step(frame);
                }

                return staged.getScene().getSprites().size();
            };

            // One at a time, for the reason the test above gives.
            const std::size_t dry = sprited("Clear", over, over);
            const std::size_t raining = sprited("Rain", over, over);
            ASSERT_GT(raining, dry) << "the rain put nothing in the air, so this proves nothing";

            EXPECT_EQ(sprited("Rain", over, under), dry) << "the drops crossed the surface and stayed";
        }
    }
}

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <boost/program_options/variables_map.hpp>

#include <components/files/configurationmanager.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/sceneutil/visitor.hpp>

#include <components/esm3/loadnpc.hpp>

#include <apps/rtxtool/actor.hpp>
#include <apps/rtxtool/npc.hpp>
#include <apps/rtxtool/world.hpp>

#include "installation.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        /// A frame's worth of elapsed time. These tests are about the animation clock, which is
        /// `pose`'s first argument; the second only drives what integrates rather than samples, and
        /// nothing being posed here has an emitter on it.
        constexpr float sStep = 1.0f / 60.0f;

        /// A cliff racer, which is a skinned creature whose animation is a wing beat: the widest
        /// travel of any vertex in the game against the shortest track, so a pose apart in time is a
        /// pose apart in space by an amount nothing has to be careful about measuring.
        constexpr VFS::Path::NormalizedView sCreature("meshes/r/cliffracer.nif");

        /// How far apart the two furthest vertices of `mesh` are between two scenes.
        ///
        /// A maximum rather than a mean, because most of a wing is near its root and barely moves;
        /// averaging a beat over the whole body is how a real animation comes out looking still.
        float travelled(const Rtx::SceneDesc& before, const Rtx::SceneDesc& after, Rtx::Index mesh)
        {
            const std::span<const osg::Vec3f> was = before.getMeshPositions(mesh);
            const std::span<const osg::Vec3f> is = after.getMeshPositions(mesh);
            EXPECT_EQ(was.size(), is.size()) << "a pose does not change how many vertices there are";

            float furthest = 0.0f;
            for (std::size_t at = 0; at < std::min(was.size(), is.size()); ++at)
                furthest = std::max(furthest, (is[at] - was[at]).length());

            return furthest;
        }

        /// An actor loads, poses, and poses differently at a different time.
        ///
        /// **The headless half of what the game window was being opened for.** A skinned body's
        /// vertices are computed during the cull traversal, so a harness that only walks the graph
        /// sees the bind pose for ever; this drives the real traversals and asserts the vertices
        /// actually moved.
        ///
        /// It is written as one test over one loaded world because opening one costs about a second:
        /// the assertions are cheap and the fixture is not.
        TEST(RtxActorTest, aCreatureLoadsPosedAndMovesBetweenPoses)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            Actor actor(*world, loadCreature(*world, sCreature), osg::Matrixf::identity());

            EXPECT_EQ(actor.getSkeleton().value(), "meshes/r/xcliffracer.nif")
                << "an actor's skeleton is a second file beside its model";
            ASSERT_GT(actor.getPosedBones(), 0u) << "keyframes that reach no bone pose nothing";
            ASSERT_GT(actor.getDuration(), 0.0f);

            // Two poses a quarter of the track apart, which for a wing beat is most of the way from
            // one extreme to the other whatever the track happens to contain.
            Rtx::SceneDesc first;
            {
                Rtx::SceneExtractor extractor(first);
                actor.pose(0.0f, sStep);
                const Rtx::ExtractionStats stats = extractor.extract(actor.getRoot(), actor.getTransform(), 0);

                EXPECT_GT(stats.mDeformed, 0u) << "a creature is skinned geometry";
                EXPECT_EQ(stats.mSkippedUnknown, 0u);
                ASSERT_GT(stats.mInstances, 0u);
            }

            Rtx::SceneDesc second;
            {
                Rtx::SceneExtractor extractor(second);
                actor.pose(actor.getDuration() * 0.25f, sStep);
                extractor.extract(actor.getRoot(), actor.getTransform(), 0);
            }

            ASSERT_EQ(first.getMeshes().size(), second.getMeshes().size());

            float furthest = 0.0f;
            for (Rtx::Index mesh = 0; mesh < first.getMeshes().size(); ++mesh)
                furthest = std::max(furthest, travelled(first, second, mesh));

            // A Morrowind unit is about seven centimetres and a cliff racer's wings span some three
            // hundred of them, so a quarter of a beat is tens of units. Ten is well under anything a
            // beat does and well over anything rounding could produce.
            EXPECT_GT(furthest, 10.0f) << "the two poses are the same pose";

            // And back where it started, which is the assertion that says the pose is a function of
            // the time rather than a walk that accumulates: an actor stepped round its own loop has
            // to come back, or a long run drifts.
            Rtx::SceneDesc again;
            {
                Rtx::SceneExtractor extractor(again);
                actor.pose(actor.getDuration(), sStep);
                extractor.extract(actor.getRoot(), actor.getTransform(), 0);
            }

            float drifted = 0.0f;
            for (Rtx::Index mesh = 0; mesh < first.getMeshes().size(); ++mesh)
                drifted = std::max(drifted, travelled(first, again, mesh));

            EXPECT_LT(drifted, 1e-3f) << "a whole track later is the pose it started in";
        }

        /// What one assembled person came to.
        struct Assembled
        {
            VFS::Path::Normalized mSkeleton;
            std::uint32_t mInstances = 0;
            std::uint32_t mDeformed = 0;

            /// What they are made of, and not merely how many pieces. Two people can be the same
            /// number of pieces and a robe is not a pair of legs.
            std::uint32_t mTriangles = 0;
        };

        /// A person is assembled out of their outfit and their race's body parts.
        ///
        /// **A mirror of one has to come back with exactly what was hung on them.** Morrowind's base
        /// animation files carry placeholder geometry — the female one most visibly — and a person
        /// built without stripping it comes out wearing a magenta torso and a grey chevron nobody
        /// drew. That defect has no other shape than this: more meshes in the graph than the outfit
        /// put there.
        TEST(RtxActorTest, aPersonIsAssembledFromTheirRacesBodyParts)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            // A scene each, so the triangle count is this person's and not a running total.
            const auto assemble = [&](const char* id, bool dressed) {
                const ESM::NPC* who = findNpc(*world, id);
                EXPECT_NE(who, nullptr) << id;
                if (who == nullptr)
                    return Assembled{};

                Rtx::SceneDesc scene;
                Rtx::SceneExtractor extractor(scene);

                Actor actor(*world, buildNpc(*world, *who, dressed), osg::Matrixf::identity());
                actor.pose(0.0f, sStep);

                const Rtx::ExtractionStats stats = extractor.extract(actor.getRoot(), actor.getTransform(), 0);
                return Assembled{ actor.getSkeleton(), stats.mInstances, stats.mDeformed, scene.getTriangleCount() };
            };

            const Assembled man = assemble("madres navur", false);
            const Assembled woman = assemble("galsa gindu", false);
            const Assembled cat = assemble("ra'virr", false);

            ASSERT_GT(man.mInstances, 0u) << "a person made of nothing is not a person";
            EXPECT_EQ(man.mInstances, woman.mInstances)
                << "two naked Dunmer are the same limb for limb, so the skeleton brought geometry of its own";

            // And what somebody carries is what they wear. One woman against herself rather than
            // against somebody else, and by what she is made of rather than by how many pieces:
            // a skirt claims the slots two legs had, so the piece count barely moves and the
            // triangles cannot help but.
            EXPECT_NE(assemble("galsa gindu", true).mTriangles, woman.mTriangles)
                << "her own clothes made no difference to her";

            // **Some of a person is skinned and most of one is not**, which is worth pinning because
            // it decides how they move: a forearm is a rigid mesh parented to the forearm bone and
            // rides the transform, and only the pieces that span a joint — the chest, the hands —
            // are deformed vertex by vertex. Both routes have to work or half a person animates.
            EXPECT_GT(woman.mDeformed, 0u) << "the parts that span a joint are skinned";
            EXPECT_LT(woman.mDeformed, woman.mInstances) << "and the rest ride their bone";

            // Three races and sexes, three skeletons — and the beast one serves both sexes, which is
            // Morrowind's own arrangement rather than something to correct.
            EXPECT_NE(man.mSkeleton, woman.mSkeleton) << "a woman is not built on a man's skeleton";
            EXPECT_NE(cat.mSkeleton, man.mSkeleton) << "and a Khajiit is built on neither";
            EXPECT_NE(cat.mSkeleton, woman.mSkeleton);
        }

        /// A weapon goes in the hand, and its kind decides the stance the hand is in.
        ///
        /// **A weapon is not a body part and does not go through the paper doll.** It is the item's
        /// own model hung on "Weapon Bone", and the bone is only *placed* by the idle that goes with
        /// the weapon — the empty-handed one is the idle with nothing in that hand, and a sword hung
        /// there comes out lying across its owner. So what is asserted is both: something arrived on
        /// the bone, and the person is standing in the right way to be holding it.
        ///
        /// A shield needs none of this and is not tested here: all sixty-five of the shipped shields
        /// name a `PRT_Shield` body part, so one goes on with the rest of the wardrobe.
        TEST(RtxNpcTest, aWeaponHangsInTheHandAndItsKindSetsTheStance)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            /// What hangs on the weapon bone, and what stance was chosen to hold it in.
            struct Armed
            {
                unsigned int mHeld = 0;
                std::string mIdle;
                std::string mFallback;
            };

            const auto arm = [&](const char* id, bool dressed) {
                const ESM::NPC* who = findNpc(*world, id);
                EXPECT_NE(who, nullptr) << id;
                if (who == nullptr)
                    return Armed{};

                const ActorModel built = buildNpc(*world, *who, dressed);

                SceneUtil::NodeMap bones;
                SceneUtil::NodeMapVisitor collect(bones);
                built.mRoot->accept(collect);

                const auto found = bones.find("Weapon Bone");
                EXPECT_NE(found, bones.end()) << "the base animation has no weapon bone";
                return Armed{ found == bones.end() ? 0u : found->second->getNumChildren(), built.mIdle,
                    built.mIdleFallback };
            };

            // With nothing equipped the bone is as the skeleton shipped it — one child, the "Weapon"
            // bone under it — and there is nothing to stand in but the plain idle.
            const Armed bare = arm("Afer Flaccus_guard", false);
            EXPECT_EQ(bare.mIdle, "idle");
            EXPECT_EQ(bare.mFallback, "idle");

            // A guard with a long blade and an archer with a bow. Both hang on the same bone —
            // the game's table sends a bow to a left-hand one and vanilla's skeleton has not got it
            // — and what tells the two apart is what they are standing in.
            const Armed blade = arm("Afer Flaccus_guard", true);
            const Armed bow = arm("alveleg", true);

            EXPECT_EQ(blade.mHeld, bare.mHeld + 1) << "the sword is not in his hand";
            EXPECT_EQ(bow.mHeld, bare.mHeld + 1) << "the bow is not in her hand";

            EXPECT_EQ(blade.mIdle, "idle1h");
            EXPECT_EQ(blade.mFallback, "idle1h");

            EXPECT_EQ(bow.mIdle, "idlebow");
            EXPECT_NE(bow.mIdle, blade.mIdle) << "a bow is held the way a sword is";

            // Vanilla's base animation has no bow idle, and the game's own rule sends a two-handed
            // *ranged* weapon to the one-handed stance rather than the two-handed one.
            EXPECT_EQ(bow.mFallback, "idle1h");
        }
    }
}

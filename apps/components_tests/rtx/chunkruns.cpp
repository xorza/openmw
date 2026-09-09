#include <cstdint>

#include <gtest/gtest.h>

#include <components/rtx/chunkruns.hpp>

namespace Rtx
{
    namespace
    {
        Terrain::ChunkName nameAt(const float x, const float y, const float size = 1.0f)
        {
            return Terrain::ChunkName{ .mCentre = osg::Vec2f(x, y), .mSize = size };
        }

        ChunkStep stepOf(const std::size_t who, const Index mesh)
        {
            return ChunkStep{ .mWho = who, .mMesh = mesh, .mMaterial = mesh + 100, .mPlacement = mesh + 200 };
        }

        /// A run is readable on the round after the one that recorded it, and never on its own.
        TEST(RtxChunkRunsTest, aRunIsReadOnTheRoundAfterTheOneThatWroteIt)
        {
            ChunkRuns runs;

            runs.beginWalk(1);
            runs.open(nameAt(0.0f, 0.0f));
            EXPECT_TRUE(runs.getRecorded().empty());
            runs.add(stepOf(7, 3));
            runs.add(stepOf(8, 4));
            runs.close(ChunkRuns::Ended::Walked);

            // The same round again: what this walk wrote is not what it reads.
            runs.beginWalk(1);
            runs.open(nameAt(1.0f, 1.0f));
            EXPECT_TRUE(runs.getRecorded().empty());
            runs.close(ChunkRuns::Ended::Walked);

            runs.beginWalk(2);
            runs.open(nameAt(0.0f, 0.0f));
            ASSERT_EQ(runs.getRecorded().size(), 2);
            EXPECT_EQ(runs.getRecorded()[0], stepOf(7, 3));
            EXPECT_EQ(runs.getRecorded()[1], stepOf(8, 4));
            runs.add(stepOf(7, 3));
            runs.add(stepOf(8, 4));
            runs.close(ChunkRuns::Ended::Walked);
        }

        /// A replayed chunk keeps its run, so the round after it can be spared the walk as well.
        TEST(RtxChunkRunsTest, aReplayedChunkCarriesItsRunForward)
        {
            ChunkRuns runs;

            runs.beginWalk(1);
            runs.open(nameAt(0.0f, 0.0f));
            runs.add(stepOf(7, 3));
            runs.add(stepOf(8, 4));
            runs.close(ChunkRuns::Ended::Walked);

            // Three rounds that each replay what the one before them held, and never walk again.
            for (std::uint64_t round = 2; round <= 4; ++round)
            {
                runs.beginWalk(round);
                runs.open(nameAt(0.0f, 0.0f));
                ASSERT_EQ(runs.getRecorded().size(), 2) << "a replayed run was forgotten at round " << round;
                EXPECT_EQ(runs.getRecorded()[0], stepOf(7, 3));
                EXPECT_EQ(runs.getRecorded()[1], stepOf(8, 4));
                runs.close(ChunkRuns::Ended::Replayed);
            }
        }

        /// Every field of the name separates one chunk from another.
        TEST(RtxChunkRunsTest, aChunkIsFoundByEveryFieldOfItsName)
        {
            const Terrain::ChunkName recorded{
                .mCentre = osg::Vec2f(4.0f, -2.0f),
                .mSize = 0.5f,
                .mLodFlags = 3,
                .mActiveGrid = true,
            };

            ChunkRuns runs;
            runs.beginWalk(1);
            runs.open(recorded);
            runs.add(stepOf(1, 1));
            runs.close(ChunkRuns::Ended::Walked);
            runs.beginWalk(2);

            for (Terrain::ChunkName other : {
                     Terrain::ChunkName{
                         .mCentre = osg::Vec2f(4.5f, -2.0f), .mSize = 0.5f, .mLodFlags = 3, .mActiveGrid = true },
                     Terrain::ChunkName{
                         .mCentre = osg::Vec2f(4.0f, -2.5f), .mSize = 0.5f, .mLodFlags = 3, .mActiveGrid = true },
                     Terrain::ChunkName{
                         .mCentre = osg::Vec2f(4.0f, -2.0f), .mSize = 1.0f, .mLodFlags = 3, .mActiveGrid = true },
                     Terrain::ChunkName{
                         .mCentre = osg::Vec2f(4.0f, -2.0f), .mSize = 0.5f, .mLodFlags = 4, .mActiveGrid = true },
                     Terrain::ChunkName{
                         .mCentre = osg::Vec2f(4.0f, -2.0f), .mSize = 0.5f, .mLodFlags = 3, .mActiveGrid = false },
                 })
            {
                runs.open(other);
                EXPECT_TRUE(runs.getRecorded().empty()) << "a chunk was found under a name that is not its own";
                runs.close(ChunkRuns::Ended::Walked);
            }

            runs.open(recorded);
            EXPECT_EQ(runs.getRecorded().size(), 1);
            runs.close(ChunkRuns::Ended::Replayed);
        }

        /// Two chunks of one round keep their own runs, whatever order they are read back in.
        TEST(RtxChunkRunsTest, everyChunkOfARoundKeepsItsOwnRun)
        {
            ChunkRuns runs;

            runs.beginWalk(1);
            runs.open(nameAt(0.0f, 0.0f));
            runs.add(stepOf(1, 1));
            runs.close(ChunkRuns::Ended::Walked);
            runs.open(nameAt(1.0f, 0.0f));
            runs.add(stepOf(2, 2));
            runs.add(stepOf(3, 3));
            runs.close(ChunkRuns::Ended::Walked);
            runs.open(nameAt(2.0f, 0.0f));
            runs.close(ChunkRuns::Ended::Walked);

            runs.beginWalk(2);

            // Read back out of the order they were written in, and replayed, so the runs the round
            // carries forward are interleaved with the reads of the round before it.
            runs.open(nameAt(1.0f, 0.0f));
            ASSERT_EQ(runs.getRecorded().size(), 2);
            EXPECT_EQ(runs.getRecorded()[0], stepOf(2, 2));
            EXPECT_EQ(runs.getRecorded()[1], stepOf(3, 3));
            runs.close(ChunkRuns::Ended::Replayed);

            // A chunk that placed nothing has nothing to stamp, so it is walked however often it
            // is met.
            runs.open(nameAt(2.0f, 0.0f));
            EXPECT_TRUE(runs.getRecorded().empty()) << "a chunk with no drawables read another chunk's run";
            EXPECT_FALSE(runs.canReplay());
            runs.close(ChunkRuns::Ended::Walked);

            runs.open(nameAt(0.0f, 0.0f));
            ASSERT_EQ(runs.getRecorded().size(), 1);
            EXPECT_EQ(runs.getRecorded()[0], stepOf(1, 1));
            runs.close(ChunkRuns::Ended::Replayed);

            // And what that round carried forward is what the next one reads.
            runs.beginWalk(3);
            runs.open(nameAt(1.0f, 0.0f));
            ASSERT_EQ(runs.getRecorded().size(), 2);
            EXPECT_EQ(runs.getRecorded()[0], stepOf(2, 2));
            runs.close(ChunkRuns::Ended::Replayed);
            runs.open(nameAt(0.0f, 0.0f));
            ASSERT_EQ(runs.getRecorded().size(), 1);
            EXPECT_EQ(runs.getRecorded()[0], stepOf(1, 1));
            runs.close(ChunkRuns::Ended::Replayed);
        }

        /// A name handed over twice in one round keeps one run, and leaves nothing behind.
        ///
        /// **A stop that walks the graph twice is what does this**, and both walks produce the same
        /// run — so what the second must not do is grow the buffer with steps no run names.
        TEST(RtxChunkRunsTest, aNameHandedOverTwiceInARoundKeepsOneRun)
        {
            ChunkRuns runs;

            runs.beginWalk(1);
            for (int walk = 0; walk < 2; ++walk)
            {
                runs.open(nameAt(0.0f, 0.0f));
                runs.add(stepOf(1, 1));
                runs.add(stepOf(2, 2));
                runs.close(ChunkRuns::Ended::Walked);

                runs.open(nameAt(1.0f, 0.0f));
                runs.add(stepOf(3, 3));
                runs.close(ChunkRuns::Ended::Walked);
            }

            runs.beginWalk(2);

            runs.open(nameAt(0.0f, 0.0f));
            ASSERT_EQ(runs.getRecorded().size(), 2);
            EXPECT_EQ(runs.getRecorded()[0], stepOf(1, 1));
            EXPECT_EQ(runs.getRecorded()[1], stepOf(2, 2));
            runs.close(ChunkRuns::Ended::Replayed);

            // The second chunk still reads its own run, which says the first pair did not shift it.
            runs.open(nameAt(1.0f, 0.0f));
            ASSERT_EQ(runs.getRecorded().size(), 1);
            EXPECT_EQ(runs.getRecorded()[0], stepOf(3, 3));
            runs.close(ChunkRuns::Ended::Replayed);
        }

        /// A chunk that held something no step describes is walked for ever after.
        TEST(RtxChunkRunsTest, aRefusedChunkIsNeverReplayed)
        {
            ChunkRuns runs;

            runs.beginWalk(1);
            runs.open(nameAt(0.0f, 0.0f));
            runs.add(stepOf(1, 1));
            runs.refuse();
            runs.add(stepOf(2, 2));
            runs.close(ChunkRuns::Ended::Walked);

            runs.open(nameAt(1.0f, 0.0f));
            runs.add(stepOf(3, 3));
            runs.close(ChunkRuns::Ended::Walked);

            // The refusal reaches the round after it, and the chunk beside it is untouched.
            runs.beginWalk(2);

            runs.open(nameAt(0.0f, 0.0f));
            EXPECT_EQ(runs.getRecorded().size(), 2) << "a refused chunk lost the run it recorded";
            EXPECT_FALSE(runs.canReplay());
            runs.add(stepOf(1, 1));
            runs.add(stepOf(2, 2));
            runs.close(ChunkRuns::Ended::Walked);

            runs.open(nameAt(1.0f, 0.0f));
            EXPECT_TRUE(runs.canReplay());
            runs.close(ChunkRuns::Ended::Replayed);
        }

        /// A chunk absent for one round is a chunk with nothing recorded for it.
        TEST(RtxChunkRunsTest, aChunkNotMetForARoundIsForgotten)
        {
            ChunkRuns runs;

            runs.beginWalk(1);
            runs.open(nameAt(0.0f, 0.0f));
            runs.add(stepOf(1, 1));
            runs.close(ChunkRuns::Ended::Walked);

            runs.beginWalk(2);
            runs.open(nameAt(9.0f, 9.0f));
            runs.close(ChunkRuns::Ended::Walked);

            runs.beginWalk(3);
            runs.open(nameAt(0.0f, 0.0f));
            EXPECT_TRUE(runs.getRecorded().empty());
            runs.close(ChunkRuns::Ended::Walked);
        }

        /// `clear` forgets both rounds, so nothing recorded against a scene that has gone survives.
        TEST(RtxChunkRunsTest, clearForgetsWhatEitherRoundHeld)
        {
            ChunkRuns runs;

            runs.beginWalk(1);
            runs.open(nameAt(0.0f, 0.0f));
            runs.add(stepOf(1, 1));
            runs.close(ChunkRuns::Ended::Walked);

            runs.clear();

            runs.beginWalk(2);
            runs.open(nameAt(0.0f, 0.0f));
            EXPECT_TRUE(runs.getRecorded().empty());
            runs.close(ChunkRuns::Ended::Walked);
        }
    }
}

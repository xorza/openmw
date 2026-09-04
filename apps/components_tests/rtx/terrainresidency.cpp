#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include <osg/Group>
#include <osg/NodeVisitor>

#include <components/esm3/loadcell.hpp>
#include <components/loadinglistener/reporter.hpp>
#include <components/rtx/terrainresidency.hpp>
#include <components/terrain/view.hpp>
#include <components/terrain/world.hpp>

namespace Rtx
{
    namespace
    {
        /// How many chunks a warming pass has to get through, and how long each one takes.
        ///
        /// **Long enough that a frame cannot wait one out by accident.** The whole pass is forty
        /// milliseconds against a collect's two tenths of one, so a run in which no pass was ever cut
        /// short is a run in which the frames waited.
        constexpr int sChunks = 400;
        constexpr auto sChunkTime = std::chrono::microseconds(100);
        constexpr auto sCollectTime = std::chrono::microseconds(200);

        /// What the rest of a frame takes — the trace, in a harness that has none.
        ///
        /// **The thread's whole working window, and so part of the fixture.** `collect` is a slice of a
        /// frame and the warming runs in what is left of one, so a loop that called `collect` back to
        /// back would cut every pass at its first chunk and never let the two overlap at all.
        constexpr auto sRestOfFrame = std::chrono::milliseconds(1);

        /// Frames the test drives. Each moves the eye, so each asks the thread for a new square.
        constexpr int sFrames = 30;

        /// A view that only has to exist: nothing here resolves a quad tree into one.
        struct EmptyView : Terrain::View
        {
            void reset() override {}
        };

        /// A terrain that says when somebody is inside its builder, and takes its time in there.
        ///
        /// **What it stands in for is `Terrain::QuadTreeWorld::loadRenderingNode`**, which `collect` and
        /// `preload` both reach and whose chunk caches read, build and write back without holding
        /// anything. Two callers in there at once is the defect: they both miss one key, they both build
        /// it, the later write wins, and the frame keeps whichever node its own call returned. So this
        /// counts who is inside rather than reproducing what goes wrong in there.
        class WatchedTerrain final : public Terrain::World
        {
        public:
            explicit WatchedTerrain(osg::Group* parent)
                : Terrain::World(parent, nullptr, ~0u, ESM::Cell::sDefaultWorldspaceId)
            {
            }

            Terrain::View* createView() override { return new EmptyView; }

            void preload(Terrain::View*, const osg::Vec3f&, const osg::Vec4i&, std::atomic<bool>& abort,
                Loading::Reporter&) override
            {
                const Inside held(*this);
                ++mPasses;

                for (int chunk = 0; chunk < sChunks; ++chunk)
                {
                    // Where `QuadTreeWorld::preload` reads it: between one chunk and the next.
                    if (abort)
                    {
                        ++mCutShort;
                        return;
                    }

                    std::this_thread::sleep_for(sChunkTime);
                }
            }

            void collect(Terrain::View*, const osg::Vec3f&, osg::NodeVisitor&) override
            {
                const Inside held(*this);
                std::this_thread::sleep_for(sCollectTime);
            }

            bool overlapped() const { return mOverlapped; }
            std::uint32_t getPasses() const { return mPasses; }
            std::uint32_t getCutShort() const { return mCutShort; }

        private:
            /// One caller's stay inside the builder, which is what makes a second one visible.
            struct Inside
            {
                explicit Inside(WatchedTerrain& terrain)
                    : mTerrain(terrain)
                {
                    if (mTerrain.mBuilders.fetch_add(1) != 0)
                        mTerrain.mOverlapped = true;
                }

                ~Inside() { --mTerrain.mBuilders; }

                Inside(const Inside&) = delete;
                Inside& operator=(const Inside&) = delete;

                WatchedTerrain& mTerrain;
            };

            std::atomic<int> mBuilders{ 0 };
            std::atomic<bool> mOverlapped{ false };
            std::atomic<std::uint32_t> mPasses{ 0 };
            std::atomic<std::uint32_t> mCutShort{ 0 };
        };

        /// The frame and the warming thread take the terrain's builder in turn, and the frame waits for
        /// one chunk of a pass rather than for the whole of it.
        ///
        /// **A chunk built twice is a chunk with two identities, and the mirror reads identity.** What
        /// the race cost was not a wasted build: the frame kept whichever node its own `getChunk`
        /// returned while the cache held the other, so the next frame met a different drawable and the
        /// scene came out with a different mesh, a different material and a different instance count.
        /// Five runs of `bench` at Seyda Neen's shore placed between 15,959 and 15,964 instances. With
        /// the two held apart all five agree exactly.
        ///
        /// **`ChunkManager` makes it a picture too**: a chunk with no entry of its own takes its passes
        /// and its composite map from whatever chunk of the same centre and level the cache happens to
        /// hold, so which thread built first decided what the distant ground was drawn with.
        ///
        /// **Both claims on one run, because holding them apart is what the yield costs.** A lock with
        /// nothing else would put a whole warming pass in front of every `collect` that arrived during
        /// one — the spike the warming exists to remove — and that failure and the overlap are two
        /// readings of the same thirty frames.
        TEST(RtxTerrainResidencyTest, theBuilderIsTakenInTurnAndAFrameWaitsOneChunkOfAPass)
        {
            const osg::ref_ptr<osg::Group> parent = new osg::Group;

            // Declared first, so the residency below is destroyed first and joins the thread that is
            // holding this terrain.
            WatchedTerrain terrain(parent);

            {
                TerrainResidency resident;
                resident.follow(&terrain);

                osg::NodeVisitor visitor;
                for (int frame = 0; frame < sFrames; ++frame)
                {
                    // Moved every frame, because an eye that stands still is one `ask` skips.
                    resident.setViewPoint(osg::Vec3f(static_cast<float>(frame) * 64.0f, 0.0f, 0.0f));
                    resident.collect(visitor);
                    std::this_thread::sleep_for(sRestOfFrame);
                }
            }

            ASSERT_GT(terrain.getPasses(), 0u) << "the warming thread never ran, so this proves nothing";
            EXPECT_FALSE(terrain.overlapped()) << "the frame and the warming thread were in the builder at once";
            EXPECT_GT(terrain.getCutShort(), 0u) << "no pass was cut short, so the frames waited them out";
        }
    }
}

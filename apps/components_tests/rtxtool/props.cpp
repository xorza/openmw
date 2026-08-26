#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <osg/Group>

#include <gtest/gtest.h>

#include <boost/program_options/variables_map.hpp>

#include <components/esm3/loadcell.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>

#include <apps/rtxtool/cellscene.hpp>
#include <apps/rtxtool/posedactors.hpp>
#include <apps/rtxtool/world.hpp>

#include "installation.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        /// The fullest interior the shipped content has: fifty-five emitters in one room, which is
        /// where the reference implementation measured the cost of drawing them.
        constexpr std::string_view sCell = "Seyda Neen, Census and Excise Office";

        std::vector<osg::Vec3f> spritePositions(const Rtx::SceneDesc& scene)
        {
            std::vector<osg::Vec3f> positions;
            positions.reserve(scene.getSprites().size());
            for (const Rtx::Sprite& sprite : scene.getSprites())
                positions.push_back(sprite.mPosition);

            std::sort(
                positions.begin(), positions.end(), [](const osg::Vec3f& a, const osg::Vec3f& b) { return a < b; });
            return positions;
        }

        /// How many of `now`'s sprites stand where none of `before`'s did.
        ///
        /// Sorted and differenced rather than compared index by index, because a particle is born
        /// into whichever slot last fell free: an emitter that is running renumbers itself, and a
        /// pairwise comparison would call that movement whether or not anything moved.
        std::size_t moved(const std::vector<osg::Vec3f>& before, const std::vector<osg::Vec3f>& now)
        {
            std::vector<osg::Vec3f> fresh;
            std::set_difference(now.begin(), now.end(), before.begin(), before.end(), std::back_inserter(fresh),
                [](const osg::Vec3f& a, const osg::Vec3f& b) { return a < b; });

            return fresh.size();
        }

        /// A cell's emitters are placed by the live props and by nothing else, and they run.
        ///
        /// **A template is one object handed to every reference and nothing ever updates it**, so
        /// the emitters in it hold the handful of particles the file was saved with for ever. The
        /// still walk therefore reports its emitter-carrying references instead of mirroring them,
        /// and an instance of each is what actually burns.
        ///
        /// The assertion is that the sprites *move*, not that there are more of them: both paths
        /// fill the emitter's quota, so the counts agree and the count says nothing.
        TEST(RtxLivePropsTest, aCellsEmittersArePlacedByTheLivePropsAndRun)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            const ESM::Cell* cell = world->findCell(std::string(sCell));
            ASSERT_NE(cell, nullptr);

            // What one walk of the shared templates gives: every emitter in the room, each holding
            // its authored seed.
            Rtx::SceneDesc seeded;
            Rtx::ExtractionStats seedStats;
            {
                osg::ref_ptr<osg::Group> root = new osg::Group;
                Rtx::SceneExtractor extractor(seeded);
                LoadedCells loaded;
                readRegion(*world, *cell, *root, seeded, extractor, loaded, /*liveProps=*/false);
                seedStats = extractor.extract(*root, osg::Matrixf::identity(), 0);
            }

            ASSERT_GT(seedStats.mEmitters, 0u) << "the room is full of candles";

            Rtx::SceneDesc live;
            osg::ref_ptr<osg::Group> root = new osg::Group;
            Rtx::SceneExtractor extractor(live);
            LoadedCells loaded;
            const CellReport report = readRegion(*world, *cell, *root, live, extractor, loaded, /*liveProps=*/true);
            const Rtx::ExtractionStats mirrored = extractor.extract(*root, osg::Matrixf::identity(), 0);

            EXPECT_EQ(mirrored.mEmitters, 0u) << "a reference that is going to be instanced is not mirrored too";
            ASSERT_FALSE(report.mProps.empty());

            const ActorRequest request{ .mResidents = false, .mProps = true };
            PosedActors posed(*world, live, extractor, *root, request);
            posed.addProps(report.mProps);
            const Rtx::ExtractionStats settled = posed.settle();

            EXPECT_EQ(posed.getPropCount(), report.mProps.size());
            EXPECT_EQ(settled.mEmitters, seedStats.mEmitters) << "the same emitters, instanced rather than shared";

            const std::vector<osg::Vec3f> first = spritePositions(live);
            ASSERT_FALSE(first.empty());

            // Half a second at the rate a frame advances. Every emitter in this room has a lifetime
            // under a second, so by here most of what was alive has died and been replaced.
            for (std::uint32_t frame = 1; frame <= 30; ++frame)
                ASSERT_TRUE(posed.step(frame));

            const std::vector<osg::Vec3f> later = spritePositions(live);
            EXPECT_GT(moved(first, later), later.size() / 2)
                << "the emitters are frozen: " << later.size() << " sprites and " << moved(first, later)
                << " of them somewhere new";

            // **And they survive the crossing that takes everyone out of the scene.** `unplace`
            // empties the lists a walk refills — sprites and emitters among them — and only the
            // lights are put back by hand; everything else has to come from the walk. That was a
            // defect while the harness mirrored only its actors, and mirroring the whole graph is
            // what fixed it, so this is the assertion that says so rather than a comment claiming it.
            posed.unplace();
            EXPECT_EQ(live.getEmitters().size(), std::size_t{ 0 }) << "unplace is meant to empty them";

            const Rtx::ExtractionStats remirrored = posed.mirror();
            EXPECT_EQ(remirrored.mEmitters, settled.mEmitters) << "the room lost its candles across a crossing";
            EXPECT_FALSE(spritePositions(live).empty());

            // And the control: a template walked twice is the same sprites twice over, which is what
            // makes the movement above the instancing rather than the extraction.
            Rtx::SceneDesc again;
            {
                osg::ref_ptr<osg::Group> twiceRoot = new osg::Group;
                Rtx::SceneExtractor twice(again);
                LoadedCells once;
                readRegion(*world, *cell, *twiceRoot, again, twice, once, /*liveProps=*/false);
                twice.extract(*twiceRoot, osg::Matrixf::identity(), 0);
            }

            EXPECT_EQ(spritePositions(again), spritePositions(seeded));
        }
    }
}

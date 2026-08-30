#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include <osg/Group>

#include <gtest/gtest.h>

#include <boost/program_options/variables_map.hpp>

#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>

#include <apps/rtxtool/cellscene.hpp>
#include <apps/rtxtool/world.hpp>

#include "installation.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        /// A room lit by nothing but lights that have no mesh: four `blue_128_pulse` and four
        /// `purp_01_128_pulse`, in a cell whose ambient is fifteen over 255. Every one of the ten
        /// propylon chambers is lit this way.
        constexpr std::string_view sCell = "Berandas, Propylon Chamber";
        constexpr std::size_t sLamps = 8;

        /// A `LIGH` reference with no model still casts, exactly as the game places it.
        ///
        /// **The bug this is here for.** The placement skipped every reference whose record named
        /// no model, and a pulse light is one: it is a colour and a radius and nothing to draw. The
        /// game inserts it anyway so that the light is added (`MWClass::Light::insertObjectRendering`),
        /// and the harness rendered the chamber black.
        TEST(RtxLampsTest, aLightWithNoMeshStillBurns)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            const ESM::Cell* cell = world->findCell(std::string(sCell));
            ASSERT_NE(cell, nullptr);

            // The placement hands them over with no model and the record in hand.
            std::size_t meshless = 0;
            std::size_t withMesh = 0;
            const World::SkippedObjects skipped = world->forEachObject(*cell, [&](const World::Object& object) {
                if (object.mLight == nullptr)
                    return;
                (object.mModel.empty() ? meshless : withMesh) += 1;
            });

            EXPECT_EQ(meshless, sLamps);
            EXPECT_EQ(withMesh, std::size_t{ 0 }) << "the chamber has no candle to confuse the count with";

            // And they are not what the marker count is made of.
            EXPECT_EQ(skipped.mNoModel, std::size_t{ 0 }) << "the lights were counted as markers";

            // And they reach the scene as lights, which is what the picture is made of.
            osg::ref_ptr<osg::Group> root = new osg::Group;
            Rtx::SceneDesc scene;
            Rtx::SceneExtractor extractor(scene);
            LoadedCells loaded;
            readRegion(*world, *cell, *root, loaded, /*liveProps=*/false);
            const Rtx::ExtractionStats stats = extractor.extract(*root, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mLights, sLamps);
            EXPECT_EQ(scene.getLights().size(), sLamps) << "nothing in the chamber glows for itself";
        }

        /// A record flagged off by default places its mesh and no light, exactly as the game does.
        ///
        /// **Two lamps in a storeroom, one of them unlit.** `MWClass::Light::insertObjectRendering`
        /// builds no light source for an `OffDefault` record, so the game's graph holds one lamp
        /// here; the harness put both into its own graph, and a `LightSource` carries no flag the
        /// mirror could have read the refusal off. The counts come off the records themselves, so
        /// the assertion is the rule and not a number remembered about the cell.
        ///
        /// Asserted on the `LightSource`s the walk met and not on the scene's light table: the
        /// unlit torch is `light_torch10.nif` with its flame still in it, and a glowing surface with
        /// no `LightSource` over it is given a lamp of its own — in the game's graph as in this one.
        TEST(RtxLampsTest, aLightOffByDefaultIsNotPlaced)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            const ESM::Cell* cell = world->findCell("Balmora, Drarayne Thelas' Storage");
            ASSERT_NE(cell, nullptr);

            std::size_t burning = 0;
            std::size_t unlit = 0;
            world->forEachObject(*cell, [&](const World::Object& object) {
                if (object.mLight == nullptr)
                    return;
                ((object.mLight->mData.mFlags & ESM::Light::OffDefault) != 0 ? unlit : burning) += 1;
            });

            ASSERT_EQ(unlit, std::size_t{ 1 }) << "the storeroom no longer holds its unlit lamp";
            ASSERT_EQ(burning, std::size_t{ 1 });

            osg::ref_ptr<osg::Group> root = new osg::Group;
            Rtx::SceneDesc scene;
            Rtx::SceneExtractor extractor(scene);
            LoadedCells loaded;
            readRegion(*world, *cell, *root, loaded, /*liveProps=*/false);
            const Rtx::ExtractionStats stats = extractor.extract(*root, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mLights, burning) << "the unlit lamp was placed";
        }
    }
}

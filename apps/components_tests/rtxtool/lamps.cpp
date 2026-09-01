#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Group>

#include <gtest/gtest.h>

#include <components/esm3/loadcell.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>

#include <apps/rtxtool/cellscene.hpp>
#include <apps/rtxtool/content.hpp>
#include <apps/rtxtool/posedactors.hpp>
#include <apps/rtxtool/stagedworld.hpp>
#include <apps/rtxtool/world.hpp>

#include "installation.hpp"

namespace RtxTool
{
    namespace
    {
        /// A room lit by nothing but lights that have no mesh: four `blue_128_pulse` and four
        /// `purp_01_128_pulse`, in a cell whose ambient is fifteen over 255. Every one of the ten
        /// propylon chambers is lit this way.
        constexpr std::string_view sCell = "Berandas, Propylon Chamber";
        constexpr std::size_t sLamps = 8;

        struct RtxLampsTest : InstallationTest
        {
        };

        /// A `LIGH` reference with no model still casts, exactly as the game places it.
        ///
        /// **The bug this is here for.** The placement skipped every reference whose record named
        /// no model, and a pulse light is one: it is a colour and a radius and nothing to draw. The
        /// game inserts it anyway so that the light is added (`MWClass::Light::insertObjectRendering`),
        /// and the harness rendered the chamber black.
        TEST_F(RtxLampsTest, aLightWithNoMeshStillBurns)
        {
            const ESM::Cell* cell = getContent().findCell(std::string(sCell));
            ASSERT_NE(cell, nullptr);

            // The placement hands them over with no model and the record in hand.
            std::size_t meshless = 0;
            std::size_t withMesh = 0;
            const Content::SkippedObjects skipped
                = getContent().forEachObject(*cell, [&](const Content::Object& object) {
                      if (!object.mLight.has_value())
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
            readRegion(getWorld(), *cell, *root, loaded, /*liveProps=*/false);
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
        ///
        /// **Counted through `Rtx::castsWherePlaced`, which is not the tautology it looks like.**
        /// What that rule says about a flag is pinned against the flags themselves by
        /// `RtxLightBuilderTest`; what this asks is whether the graph route obeys it, so spelling
        /// the flag out here again would be a reading that can drift from the one the graph uses —
        /// and a drift would fail this test in a place that had nothing to do with it.
        TEST_F(RtxLampsTest, aLightOffByDefaultIsNotPlaced)
        {
            const ESM::Cell* cell = getContent().findCell("Balmora, Drarayne Thelas' Storage");
            ASSERT_NE(cell, nullptr);

            std::size_t burning = 0;
            std::size_t unlit = 0;
            getContent().forEachObject(*cell, [&](const Content::Object& object) {
                if (!object.mLight.has_value())
                    return;
                (Rtx::castsWherePlaced(*object.mLight) ? burning : unlit) += 1;
            });

            ASSERT_EQ(unlit, std::size_t{ 1 }) << "the storeroom no longer holds its unlit lamp";
            ASSERT_EQ(burning, std::size_t{ 1 });

            osg::ref_ptr<osg::Group> root = new osg::Group;
            Rtx::SceneDesc scene;
            Rtx::SceneExtractor extractor(scene);
            LoadedCells loaded;
            readRegion(getWorld(), *cell, *root, loaded, /*liveProps=*/false);
            const Rtx::ExtractionStats stats = extractor.extract(*root, osg::Matrixf::identity(), 0);

            EXPECT_EQ(stats.mLights, burning) << "the unlit lamp was placed";
        }

        /// A room whose lamps carry a flame, and how many of them hang on an `AttachLight` node.
        ///
        /// Counted off the room rather than off the rule, because there is no rule: whether a model
        /// carries that node is a fact about the file, and this is the count of the 26 lights here
        /// that stand anywhere but at the reference the cell placed.
        constexpr std::string_view sRoom = "Seyda Neen, Census and Excise Office";
        constexpr std::size_t sWicks = 12;

        /// Every light a staged room holds, in an order two stagings can be compared in.
        ///
        /// Sorted, because what the two runs must agree about is the set of places a lamp stands.
        /// The order the walk meets them in is the order the props were instanced, and instancing
        /// them is what the mode under test decides.
        std::vector<osg::Vec3f> lampPositions(const Rtx::SceneDesc& scene)
        {
            std::vector<osg::Vec3f> positions;
            positions.reserve(scene.getLights().size());
            for (const Rtx::Light& light : scene.getLights())
                positions.push_back(light.mPosition);

            std::sort(
                positions.begin(), positions.end(), [](const osg::Vec3f& a, const osg::Vec3f& b) { return a < b; });
            return positions;
        }

        /// How many of `lamps` stand clear of every `LIGH` reference's own origin.
        ///
        /// **Which is the whole of what an `AttachLight` node is for.** A candle's wick and a
        /// lantern's flame are tens of units from the reference the cell placed, so a room in which
        /// nothing stands clear is a room whose lights all fell back to their origins.
        std::size_t offsetFromOrigins(
            const std::vector<osg::Vec3f>& lamps, const std::vector<osg::Vec3f>& origins, float clear)
        {
            return static_cast<std::size_t>(std::count_if(lamps.begin(), lamps.end(), [&](const osg::Vec3f& lamp) {
                return std::none_of(origins.begin(), origins.end(),
                    [&](const osg::Vec3f& origin) { return (lamp - origin).length() <= clear; });
            }));
        }

        /// A lamp stands at its wick whether or not its flame was instanced to run.
        ///
        /// **The bug this is here for.** `SceneUtil::addLight` hangs a light on the model's
        /// `AttachLight` node, and a reference whose model carries a particle emitter has that model
        /// taken out of the cell's graph and instanced by `PosedActors` instead — so there was no
        /// `AttachLight` node left for the search to find, and the light fell back to the reference's
        /// own origin. Ten of this room's 26 lamps moved, by up to 48 units, and `--props` defaults
        /// to on, so that was every `shot`, `view`, `bench` and `verify` the harness ever took.
        ///
        /// **Two stagings and not one remembered list**, because the two modes are the two answers
        /// that disagreed: with the props left as still templates the model is in the graph and the
        /// wick is found, and with them instanced it is not.
        ///
        /// The count below is what stops the comparison passing on two rooms that are both wrong.
        TEST_F(RtxLampsTest, aLampStandsAtItsWickWhetherOrNotItsFlameRuns)
        {
            const ESM::Cell* cell = getContent().findCell(std::string(sRoom));
            ASSERT_NE(cell, nullptr);

            std::vector<osg::Vec3f> origins;
            getContent().forEachObject(*cell, [&](const Content::Object& object) {
                if (object.mLight.has_value())
                    origins.push_back(object.mTransform.getTrans());
            });
            ASSERT_FALSE(origins.empty()) << "the room no longer holds a lamp";

            const StagingRequest request;

            const auto lampsWith = [&](bool liveProps) {
                ActorRequest actors;
                actors.mProps = liveProps;
                StagedWorld staged(getWorld(), *cell, request, actors);

                return lampPositions(staged.getScene());
            };

            const std::vector<osg::Vec3f> still = lampsWith(false);
            const std::vector<osg::Vec3f> live = lampsWith(true);

            ASSERT_EQ(live.size(), still.size());
            for (std::size_t at = 0; at < still.size(); ++at)
                EXPECT_EQ(live[at], still[at]) << "lamp " << at << " moved when its flame was instanced";

            // A wick is tens of units up, so a unit of clearance separates a lamp that found one
            // from a lamp that fell back. Both modes are counted, because either could be the one
            // that stopped looking.
            constexpr float sClear = 1.0f;
            EXPECT_EQ(offsetFromOrigins(still, origins, sClear), sWicks);
            EXPECT_EQ(offsetFromOrigins(live, origins, sClear), sWicks);
        }
    }
}

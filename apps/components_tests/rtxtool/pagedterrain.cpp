#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Group>
#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <apps/rtxtool/cellscene.hpp>
#include <apps/rtxtool/content.hpp>
#include <apps/rtxtool/world.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/misc/constants.hpp>
#include <components/rtx/compositequeue.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/shadingmap.hpp>
#include <components/rtx/terraincomposite.hpp>
#include <components/rtx/texturebuilder.hpp>

#include "installation.hpp"

namespace RtxTool
{
    namespace
    {
        /// An exterior with a lot of open ground in it, so the chunks are most of what is placed.
        constexpr std::string_view sOutdoors = "-2,-9";

        /// An exterior with a town in it and more of one around it, so a chunk past the active grid
        /// has something to stand on its ground.
        constexpr std::string_view sBuiltUp = "-3,-2";

        /// One cell across, in world units.
        constexpr float sCellSize = static_cast<float>(Constants::CellSizeInUnits);

        /// Places a region into `scene`, with or without the residency asked for.
        ///
        /// **A three-by-three grid around `cell` and not the cell alone**, because that is what
        /// `readRegion` loads and therefore what the active grid comes to — which is what anything
        /// measuring "past the grid" has to measure against.
        ///
        /// @param walks how many times the world is walked, each followed by the sweep a live frame
        ///        runs. One is a load; more than one is what a frame that keeps rendering does.
        void placeOutdoors(
            World& world, const ESM::Cell& cell, Rtx::SceneDesc& scene, bool ask, std::uint32_t walks = 1)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            LoadedCells loaded;
            Rtx::SceneExtractor extractor(scene);

            readRegion(RegionRequest{ world, cell, *root, loaded, /*liveProps=*/false });
            world.setTerrainViewPoint(osg::Vec3f(cell.getGridX() * sCellSize, cell.getGridY() * sCellSize, 0.0f));

            extractor.follow(ask ? world.getResidencies() : std::span<Rtx::Residency* const>());

            for (std::uint32_t at = 0; at < walks; ++at)
            {
                scene.clearPlacement();
                extractor.extractWorld(*root, osg::Matrixf::identity(), 0);

                // What a live frame does after a walk, and what makes the walk before it load-bearing:
                // anything the sweep did not meet is dropped.
                extractor.advance();
                extractor.retire();
            }
        }

        /// What one placement came to, for the things a run of this file compares between two.
        struct GroundTally
        {
            /// Placed, and pointing at no material at all. Not terrain's alone — a drawable with no
            /// state set anywhere on its path resolves to nothing either, which is why this is
            /// compared between two runs rather than expected to be nought.
            std::uint32_t mMaterialless = 0;

            std::uint32_t mChunks = 0;

            /// The widest chunk, in world units.
            float mWidest = 0.0f;
        };

        /// How far a mesh reaches along its widest horizontal axis, in the units it was built in.
        ///
        /// **What says a chunk is bigger than a cell**, which the scene carries nowhere else: a
        /// terrain chunk is placed by a translation, so its own vertices span exactly the ground it
        /// covers.
        float spanOf(const Rtx::SceneDesc& scene, Rtx::Index mesh)
        {
            const std::span<const osg::Vec3f> positions = scene.getMeshPositions(mesh);
            if (positions.empty())
                return 0.0f;

            osg::Vec3f least = positions.front();
            osg::Vec3f most = positions.front();
            for (const osg::Vec3f& at : positions)
            {
                least.x() = std::min(least.x(), at.x());
                least.y() = std::min(least.y(), at.y());
                most.x() = std::max(most.x(), at.x());
                most.y() = std::max(most.y(), at.y());
            }

            return std::max(most.x() - least.x(), most.y() - least.y());
        }

        /// Placed instances that are not ground and stand more than `reach` from `middle`, by the
        /// wider of the two horizontal axes.
        ///
        /// **Outside the active grid is the whole assertion.** This harness stands the references of
        /// the cell it loaded itself, one at a time; past that cell nothing but a chunk manager puts
        /// anything down, so a count here is a count of what the paging built. Chebyshev and not
        /// Euclidean, because the grid it is measured against is a square.
        std::uint32_t standingBeyond(const Rtx::SceneDesc& scene, const osg::Vec2f& middle, float reach)
        {
            std::uint32_t beyond = 0;

            for (const Rtx::MeshInstance& instance : scene.getInstances())
            {
                if (!instance.isPlaced() || instance.mMaterial == Rtx::sNoIndex)
                    continue;
                if (scene.getMaterials()[instance.mMaterial].mKind == Rtx::MaterialKind::Terrain)
                    continue;

                const osg::Vec3f at = instance.mTransform.getTrans();
                if (std::abs(at.x() - middle.x()) > reach || std::abs(at.y() - middle.y()) > reach)
                    ++beyond;
            }

            return beyond;
        }

        /// How far the active grid reaches from the middle of the cell a region was staged around.
        ///
        /// **One and a half cells, because `readRegion` loads three by three.** Anything further out
        /// than this was put there by a chunk manager and by nothing else.
        constexpr float sGridReach = 1.5f * sCellSize;

        /// The middle of a cell, in world units.
        osg::Vec2f middleOf(const ESM::Cell& cell)
        {
            return osg::Vec2f((cell.getGridX() + 0.5f) * sCellSize, (cell.getGridY() + 0.5f) * sCellSize);
        }

        GroundTally tallyGround(const Rtx::SceneDesc& scene)
        {
            GroundTally tally;

            for (const Rtx::MeshInstance& instance : scene.getInstances())
            {
                if (!instance.isPlaced())
                    continue;

                if (instance.mMaterial == Rtx::sNoIndex)
                {
                    ++tally.mMaterialless;
                    continue;
                }

                const Rtx::Material& material = scene.getMaterials()[instance.mMaterial];
                if (material.mKind != Rtx::MaterialKind::Terrain)
                    continue;

                ++tally.mChunks;
                tally.mWidest = std::max(tally.mWidest, spanOf(scene, instance.mMesh));
            }

            return tally;
        }

        struct RtxPagedTerrainTest : InstallationTest
        {
        };

        /// The ground a camera leaves gets its statics back.
        ///
        /// **The active grid follows the camera, and it used to only ever widen.** `readRegion`
        /// keeps the three-by-three square around the centre and `dropCellsOutside` takes the rest
        /// away, but the square the terrain was told was the union of every cell the run had ever
        /// loaded. A cell between the two answers stood in neither picture: its own references gone
        /// with the cell, and `ObjectPaging` still refusing to page what it believed was the active
        /// grid. What that looked like was ground with the trees taken off it, behind the camera,
        /// for the rest of the run.
        ///
        /// Two placements of one cell, differing in nothing but what came before: a fresh one, and
        /// one that went two cells east and came back. The statics standing past the grid have to
        /// come to the same count, because it is the same world seen from the same place.
        TEST_F(RtxPagedTerrainTest, theGroundACameraLeavesGetsItsStaticsBack)
        {
            const ESM::Cell* home = getContent().findCell(std::string(sBuiltUp));
            ASSERT_NE(home, nullptr);

            // Two cells east, so the square the camera leaves behind is clear of the one it comes
            // back to — a neighbour would share a column with home and hide half the answer.
            const ESM::Cell* away = getContent().findCell("-1,-2");
            ASSERT_NE(away, nullptr);

            getWorld().pageTerrain(true);
            getWorld().setTerrainViewDistance(2.0f * sCellSize);

            /// Walks `route`, staging each stop the way a moving camera does, and leaves the scene
            /// holding what the last stop placed.
            const auto travel = [&](std::span<const ESM::Cell* const> route, Rtx::SceneDesc& scene) {
                const osg::ref_ptr<osg::Group> root = new osg::Group;
                LoadedCells loaded;
                Rtx::SceneExtractor extractor(scene);

                for (const ESM::Cell* stop : route)
                {
                    readRegion(RegionRequest{ getWorld(), *stop, *root, loaded, /*liveProps=*/false });
                    dropCellsOutside(getWorld(), *stop, *root, loaded);

                    // Only after the first stop is there a paged world to follow.
                    extractor.follow(getWorld().getResidencies());
                    getWorld().setTerrainViewPoint(
                        osg::Vec3f(stop->getGridX() * sCellSize, stop->getGridY() * sCellSize, 0.0f));

                    scene.clearPlacement();
                    extractor.extractWorld(*root, osg::Matrixf::identity(), 0);
                    extractor.advance();
                    extractor.retire();
                }
            };

            const osg::Vec2f middle = middleOf(*home);

            const std::array<const ESM::Cell*, 1> stay{ home };
            Rtx::SceneDesc fresh;
            travel(stay, fresh);

            const std::uint32_t stood = standingBeyond(fresh, middle, sGridReach);
            ASSERT_GT(stood, 0u) << "the distant ground came up bare, so this proves nothing";

            const std::array<const ESM::Cell*, 3> roundTrip{ home, away, home };
            Rtx::SceneDesc returned;
            travel(roundTrip, returned);

            EXPECT_EQ(standingBeyond(returned, middle, sGridReach), stood)
                << "the cells the camera passed through kept their ground and lost their statics";
        }

        /// A paged world's ground reaches the mirror, and only because it was asked for.
        ///
        /// **`Terrain::QuadTreeWorld` keeps its chunks out of the scene graph.** It resolves them
        /// inside a cull, against a view keyed on the camera doing the culling, and parents them to
        /// nothing — so with `distant terrain` on, everything that walks the graph rather than
        /// culling it saw no ground, no paged objects and no grass. `Terrain::World::collect` is the
        /// way to ask instead, and this is what says it works.
        ///
        /// **Two runs of one cell, differing in nothing but whether the residency is handed over.**
        /// A count that went up for any other reason would show up as the control placing the same
        /// number, which is what the second assertion is for.
        TEST_F(RtxPagedTerrainTest, aPagedWorldsGroundReachesTheMirrorAndOnlyBecauseItWasAskedFor)
        {
            const ESM::Cell* cell = getContent().findCell(std::string(sOutdoors));
            ASSERT_NE(cell, nullptr);

            getWorld().pageTerrain(true);

            Rtx::SceneDesc asked;
            placeOutdoors(getWorld(), *cell, asked, true);
            ASSERT_FALSE(getWorld().getResidencies().empty()) << "the run did not page its terrain";

            Rtx::SceneDesc unasked;
            placeOutdoors(getWorld(), *cell, unasked, false);

            EXPECT_GT(asked.getPlacedCount(), unasked.getPlacedCount())
                << "the chunks a quad tree hides never reached the mirror";
        }

        /// A `LIGH` past the active grid lights the world, and no light is stood twice.
        ///
        /// **The lantern the paging never stands.** `Terrain::pagedType` leaves `REC_LIGH` out, so a
        /// light a cell away has no model and no node — and a renderer that walks the graph finds
        /// nothing at all to light a distant town with. `Rtx::DistantLights` reads them out of the
        /// content files instead, and the two halves of that are asserted here: they arrive, and
        /// they arrive only where the game has not already stood the real object.
        ///
        /// **Balmora, because a count has to be large enough to mean something.** A wilderness cell
        /// holds no lantern at all, and a test that passed on nought either way would say nothing.
        TEST_F(RtxPagedTerrainTest, aLightPastTheActiveGridReachesTheSceneAndNoneIsStoodTwice)
        {
            const ESM::Cell* cell = getContent().findCell(std::string(sBuiltUp));
            ASSERT_NE(cell, nullptr);

            getWorld().pageTerrain(true);

            Rtx::SceneDesc scene;
            placeOutdoors(getWorld(), *cell, scene, true);

            // What `readRegion` loads, which is what the active grid comes to.
            const int lowX = cell->getGridX() - 1;
            const int lowY = cell->getGridY() - 1;
            const int highX = cell->getGridX() + 1;
            const int highY = cell->getGridY() + 1;

            std::uint32_t beyond = 0;
            for (const Rtx::Light& light : scene.getLights())
            {
                const int x = static_cast<int>(std::floor(light.mPosition[0] / sCellSize));
                const int y = static_cast<int>(std::floor(light.mPosition[1] / sCellSize));

                if (x < lowX || y < lowY || x > highX || y > highY)
                    ++beyond;
            }

            EXPECT_GT(beyond, 100u) << "the cells past the grid stood no light";

            // **Every light in a place of its own.** One inside the grid is on the graph and the
            // walk has met it; the same one read out of the content files as well would be the same
            // lantern counted twice, and two lamps standing in one spot is what that looks like.
            std::vector<std::array<float, 3>> places;
            places.reserve(scene.getLights().size());
            for (const Rtx::Light& light : scene.getLights())
                places.push_back({ light.mPosition[0], light.mPosition[1], light.mPosition[2] });

            std::sort(places.begin(), places.end());

            EXPECT_EQ(std::adjacent_find(places.begin(), places.end()), places.end())
                << "two lights stand in one place";
        }

        /// A world that parents its chunks offers no residency, and is reached by walking.
        ///
        /// **The other half of the same rule**, and what stops the ground being placed twice: a
        /// caller that both walked the graph and asked every terrain world it knew would count
        /// `TerrainGrid`'s chunks once each way.
        TEST_F(RtxPagedTerrainTest, aGridWorldOffersNoResidencyBecauseItsChunksAreInTheGraph)
        {
            const ESM::Cell* cell = getContent().findCell(std::string(sOutdoors));
            ASSERT_NE(cell, nullptr);

            Rtx::SceneDesc scene;
            placeOutdoors(getWorld(), *cell, scene, true);

            EXPECT_TRUE(getWorld().getResidencies().empty());
            EXPECT_GT(scene.getPlacedCount(), 0u) << "a grid world's ground is found by walking, and was not";
        }

        /// The ground past the active grid carries what the content files stand on it, and only a
        /// world that pages objects carries any.
        ///
        /// **What a hillside is made of, and the thing this harness quietly did not have.**
        /// `QuadTreeWorld::loadRenderingNode` asks every registered chunk manager for its chunk and
        /// adds whatever comes back; the game registers `Terrain::ObjectPaging` beside the terrain's
        /// own under `object paging`, and this world registered nothing. With nothing to ask, only
        /// ground could answer — so a hillside a few cells out arrived bare where the same hillside
        /// inside the grid carried a town.
        ///
        /// **Three worlds and not one**, because `pageTerrain` and `pageStatics` are read when the
        /// terrain is built and a world that has built it ignores both. The three are the same cell:
        /// paged with its statics, paged without them, and the grid that pages nothing.
        TEST_F(RtxPagedTerrainTest, aPagedWorldStandsStaticsPastTheActiveGridAndAGridWorldStandsNone)
        {
            const std::unique_ptr<World> paged = openWorld();

            const ESM::Cell* cell = getContent().findCell(std::string(sBuiltUp));
            ASSERT_NE(cell, nullptr);

            const osg::Vec2f middle = middleOf(*cell);

            // Just past the grid, because every chunk wider than a cell is baked on the way in and
            // each bake costs tens of milliseconds. What is under test is that anything at all
            // arrives out there, which one ring of chunks answers as well as ten.
            paged->pageTerrain(true);
            paged->setTerrainViewDistance(2.0f * sCellSize);

            Rtx::SceneDesc withStatics;
            placeOutdoors(*paged, *cell, withStatics, true);
            ASSERT_FALSE(paged->getResidencies().empty()) << "the run did not page its terrain";

            const std::uint32_t stood = standingBeyond(withStatics, middle, sGridReach);
            EXPECT_GT(stood, 0u) << "the distant ground came up bare";

            const std::unique_ptr<World> bare = openWorld();

            bare->pageTerrain(true);
            bare->pageStatics(false);
            bare->setTerrainViewDistance(2.0f * sCellSize);

            Rtx::SceneDesc withoutStatics;
            placeOutdoors(*bare, *cell, withoutStatics, true);

            EXPECT_EQ(standingBeyond(withoutStatics, middle, sGridReach), 0u)
                << "something stood out there with the paging switched off";

            // **The same ground under both**, which is what makes the count above the statics'
            // rather than a second world's worth of everything.
            EXPECT_EQ(tallyGround(withStatics).mChunks, tallyGround(withoutStatics).mChunks);

            const std::unique_ptr<World> grid = openWorld();

            grid->pageTerrain(false);

            Rtx::SceneDesc gridded;
            placeOutdoors(*grid, *cell, gridded, true);

            EXPECT_EQ(standingBeyond(gridded, middle, sGridReach), 0u)
                << "a grid world reaches no further than the cells it was given";
        }

        /// No chunk is textured by a render target, however large the quad tree makes it.
        ///
        /// **A composite map is an `osg::Texture2D` with no image** that `CompositeMapRenderer` draws
        /// the layer stack into through OpenGL, and every chunk a cell or larger asks for one. This
        /// path initialises no OpenGL at all, so a chunk that got one arrives with a diffuse nothing
        /// can open: `resolveTerrainMaterial` finds no layer it can use, gives up, and the chunk is
        /// placed carrying `sNoIndex` where its material should be. `Terrain::sNoCompositeMap` is
        /// what stops it being asked for, and this is what says so.
        ///
        /// **Two runs of one cell differing only in how far out the quad tree may go**, because a
        /// placement with nothing to point at is not terrain's alone: a drawable with no state set
        /// anywhere on its path resolves to nothing too, and asserting nought outright would blame a
        /// render target for one of those. Distance is what may not add to the count.
        ///
        /// **The premise is asserted beside the conclusion**, and it is not the one this was written
        /// expecting. A cell staged on its own has an active grid one cell wide, which `viewing
        /// distance` of 7168 leaves at once — so the control already reaches a chunk a whole cell
        /// across, which is exactly the size a composite map is made for. Unforced, that run alone
        /// loses five placements their material and the four-cell run loses thirty-two.
        TEST_F(RtxPagedTerrainTest, noChunkIsTexturedByARenderTargetHoweverLargeItIs)
        {
            const ESM::Cell* cell = getContent().findCell(std::string(sOutdoors));
            ASSERT_NE(cell, nullptr);

            getWorld().pageTerrain(true);

            Rtx::SceneDesc grid;
            placeOutdoors(getWorld(), *cell, grid, true);
            const GroundTally near = tallyGround(grid);

            getWorld().setTerrainViewDistance(4.0f * sCellSize);

            Rtx::SceneDesc paged;
            placeOutdoors(getWorld(), *cell, paged, true);
            const GroundTally far = tallyGround(paged);

            EXPECT_GT(far.mChunks, near.mChunks) << "four cells of distance produced no more ground than none";
            EXPECT_GE(near.mWidest, sCellSize) << "the control made no chunk a composite was ever on offer for";
            EXPECT_GT(far.mWidest, near.mWidest) << "distance made nothing coarser than the grid already had";

            EXPECT_EQ(far.mMaterialless, near.mMaterialless)
                << "distance cost a placement its material, which is what a render target does";

            for (const Rtx::MeshInstance& instance : paged.getInstances())
            {
                if (!instance.isPlaced() || instance.mMaterial == Rtx::sNoIndex)
                    continue;

                const Rtx::Material& material = paged.getMaterials()[instance.mMaterial];
                if (material.mKind != Rtx::MaterialKind::Terrain)
                    continue;

                ASSERT_GT(material.mLayerCount, 0u) << "ground with nothing to draw it with";
                for (std::uint32_t at = 0; at < material.mLayerCount; ++at)
                {
                    const Rtx::MaterialLayer& layer = paged.getLayers()[material.mLayerOffset + at];

                    ASSERT_NE(layer.mDiffuse, Rtx::sNoIndex);
                    EXPECT_FALSE(paged.getTextures()[layer.mDiffuse].value().empty())
                        << "a ground layer whose diffuse came from no file";
                }
            }
        }

        /// A second walk of the same world keeps the ground the graph does not parent.
        ///
        /// **The sweep is global, so a walk that did not ask for the hidden chunks retires them.**
        /// The residency used to be an argument to `extract`, and one frame is walked by more than
        /// one owner: the harness's actor stepper and the game's precipitation pass both walk from
        /// the same extractor, and the one that walks the world without handing over what a quad tree
        /// keeps out of the graph swept every chunk the other had placed. The ground reached the
        /// mirror on the first frame and was gone on the second — a town standing on open sea, with
        /// a scene, a top level and an instance count that all looked correct.
        ///
        /// `follow` is what makes it structural: the residency is the extractor's, so no walk can be
        /// the one that forgets. This is the test that says a second walk does not undo the first.
        TEST_F(RtxPagedTerrainTest, aSecondWorldWalkKeepsTheGroundTheGraphDoesNotParent)
        {
            const ESM::Cell* cell = getContent().findCell(std::string(sOutdoors));
            ASSERT_NE(cell, nullptr);

            getWorld().pageTerrain(true);

            Rtx::SceneDesc once;
            placeOutdoors(getWorld(), *cell, once, true, 1);
            const GroundTally loaded = tallyGround(once);
            ASSERT_GT(loaded.mChunks, 0u) << "no paged ground was placed at all";

            Rtx::SceneDesc again;
            placeOutdoors(getWorld(), *cell, again, true, 3);
            const GroundTally standing = tallyGround(again);

            EXPECT_EQ(standing.mChunks, loaded.mChunks) << "walking the world again swept the ground a quad tree hides";
            EXPECT_GE(standing.mWidest, loaded.mWidest) << "the coarse chunks went and the near ones stayed";
        }

        /// Ground past a cell is drawn from one baked texture, and the uploader can read it.
        ///
        /// **The other half of `sNoCompositeMap`.** Refusing the render target left a distant chunk
        /// with its whole layer stack, which is a mask lookup and a texture fetch per layer at every
        /// hit; flattening it is what turns that back into one fetch. The slot is an image nothing
        /// can open, so what says it worked is that `SceneTextures` describes it rather than
        /// counting it unreadable and drawing the stand-in.
        ///
        /// **And the queue is what bakes it, on a thread of its own and collected here.** A chunk
        /// asks by setting `Material::mFlatten` and shades from its layer stack until one comes
        /// back, which is what keeps a cell boundary from spending a quarter of a second flattening
        /// eight of them: this asserts both halves, that nothing is flattened before the queue is
        /// collected and that everything is after — and that a collect keeps to the bound it is
        /// given, because that bound is what an arrival frame pays.
        ///
        /// A radius barely past the grid, because every composite in the scene is baked here and each
        /// one costs tens of milliseconds — the figure `plan.md` §6 records.
        TEST_F(RtxPagedTerrainTest, groundPastACellIsFlattenedIntoOneTextureTheUploaderCanRead)
        {
            const ESM::Cell* cell = getContent().findCell(std::string(sOutdoors));
            ASSERT_NE(cell, nullptr);

            getWorld().pageTerrain(true);
            getWorld().setTerrainViewDistance(1.25f * sCellSize);

            Rtx::SceneDesc scene;
            placeOutdoors(getWorld(), *cell, scene, true);

            const auto flattenedSlots = [&] {
                std::vector<Rtx::Index> slots;
                for (const Rtx::Material& material : scene.getMaterials())
                    if (material.mKind == Rtx::MaterialKind::Terrain && material.mDiffuse != Rtx::sNoIndex)
                        slots.push_back(material.mDiffuse);
                return slots;
            };

            std::uint32_t asked = 0;
            for (const Rtx::Material& material : scene.getMaterials())
                if (material.mKind == Rtx::MaterialKind::Terrain && material.mFlatten)
                    ++asked;

            ASSERT_GT(asked, 0u) << "no chunk was wide enough to be flattened";
            EXPECT_TRUE(flattenedSlots().empty()) << "a chunk was flattened by the walk that met it";

            // **A second queue that is never collected**, so there is still something waiting when the
            // scene is cleared at the end of this test.
            Rtx::CompositeQueue holding;
            holding.gather(scene, getWorld().getImageManager());
            ASSERT_EQ(holding.getWaitingCount(), asked);

            Rtx::CompositeQueue queue;
            queue.gather(scene, getWorld().getImageManager());
            EXPECT_EQ(queue.getWaitingCount(), asked);
            EXPECT_TRUE(flattenedSlots().empty()) << "a chunk was flattened by the hand-over rather than by the baker";

            // The two halves the uploader calls, in its order: wait for the baker, then take back
            // what it made. One first, which is what a frame's bound looks like, and the rest at
            // once, which is what a caller with no next frame asks for.
            queue.finish();
            EXPECT_EQ(queue.collect(scene, 1), std::size_t{ 1 }) << "a collect took more than its bound";
            EXPECT_EQ(queue.getWaitingCount(), asked - 1);
            EXPECT_EQ(flattenedSlots().size(), std::size_t{ 1 });

            EXPECT_EQ(queue.collect(scene, std::numeric_limits<std::size_t>::max()), asked - 1);
            EXPECT_EQ(queue.getWaitingCount(), 0u) << "a composite was collected and still waiting";

            const std::vector<Rtx::Index> flattened = flattenedSlots();
            ASSERT_EQ(flattened.size(), asked) << "the queue finished without flattening what it took on";

            Rtx::SceneTextures described;
            described.describeAll(scene, getWorld().getImageManager(), &queue);
            EXPECT_EQ(described.getUnreadable(), 0u) << "a composite the uploader would draw grey";

            // The whole chunk in one texel, per composite. Different ground averages to a different
            // colour, so a bake that read no mask — or read the same layer every time — comes back
            // with one answer for all of them.
            std::vector<std::uint32_t> averages;
            std::uint32_t found = 0;

            for (const Rtx::TextureData& data : described.getDescriptions())
            {
                if (std::find(flattened.begin(), flattened.end(), data.mSlot) == flattened.end())
                    continue;

                ++found;
                EXPECT_EQ(data.mFormat, Rtx::TextureFormat::Rgba8Srgb);
                EXPECT_EQ(data.mWidth, Rtx::sCompositeExtent);
                EXPECT_EQ(data.mHeight, Rtx::sCompositeExtent);
                ASSERT_EQ(data.mLevels.size(), 10u) << "512 square down to a single texel";

                // **Neutral all the way to the upload.** The light painted into each ground texture
                // came off per tile in the bake, and an estimate made from the composite's own bytes
                // would take it off a second time.
                ASSERT_EQ(data.mShading.size(), std::size_t{ Rtx::ShadingMap::sExtent } * Rtx::ShadingMap::sExtent);
                EXPECT_TRUE(std::all_of(data.mShading.begin(), data.mShading.end(), [](const float factor) {
                    return factor == 1.0f;
                })) << "a composite that would be corrected a second time at the hit";

                const Rtx::MipLevel& last = data.mLevels.back();
                ASSERT_EQ(last.mWidth, 1u);
                averages.push_back(std::to_integer<std::uint32_t>(data.mBytes[last.mOffset]) << 16
                    | std::to_integer<std::uint32_t>(data.mBytes[last.mOffset + 1]) << 8
                    | std::to_integer<std::uint32_t>(data.mBytes[last.mOffset + 2]));
            }

            EXPECT_EQ(found, flattened.size()) << "a composite the scene names and the uploader never described";

            const auto sameAsFirst
                = [&](std::uint32_t colour) { return averages.empty() || colour == averages.front(); };
            EXPECT_FALSE(std::all_of(averages.begin(), averages.end(), sameAsFirst))
                << "every chunk in the region averages to one colour, so nothing was read from the masks";

            // **The same queue over a second region, which is what a route across the island is.**
            // A request is four vectors and `gather` takes one off the spare list that `collect`
            // returned; a buffer that came back holding the last chunk's layers, images or weights
            // would bake this region's ground out of the one that has gone. The colours below are
            // the same region's, so a stale buffer is a wrong composite and not merely a slow one.
            Rtx::SceneDesc again;
            placeOutdoors(getWorld(), *cell, again, true);

            queue.gather(again, getWorld().getImageManager());
            ASSERT_EQ(queue.getWaitingCount(), asked) << "the reused queue took on a different count";

            queue.finish();
            EXPECT_EQ(queue.collect(again, std::numeric_limits<std::size_t>::max()), asked);

            Rtx::SceneTextures rebuilt;
            rebuilt.describeAll(again, getWorld().getImageManager(), &queue);

            std::vector<Rtx::Index> flattenedAgain;
            for (const Rtx::Material& material : again.getMaterials())
                if (material.mKind == Rtx::MaterialKind::Terrain && material.mDiffuse != Rtx::sNoIndex)
                    flattenedAgain.push_back(material.mDiffuse);

            std::vector<std::uint32_t> secondAverages;
            for (const Rtx::TextureData& data : rebuilt.getDescriptions())
            {
                if (std::find(flattenedAgain.begin(), flattenedAgain.end(), data.mSlot) == flattenedAgain.end())
                    continue;

                const Rtx::MipLevel& last = data.mLevels.back();
                secondAverages.push_back(std::to_integer<std::uint32_t>(data.mBytes[last.mOffset]) << 16
                    | std::to_integer<std::uint32_t>(data.mBytes[last.mOffset + 1]) << 8
                    | std::to_integer<std::uint32_t>(data.mBytes[last.mOffset + 2]));
            }

            std::sort(averages.begin(), averages.end());
            std::sort(secondAverages.begin(), secondAverages.end());
            EXPECT_EQ(secondAverages, averages) << "the second pass over one region baked different ground";

            // **Last, because it empties the scene.** `SceneDesc::clear` renumbers the material
            // table and a bake outlives the frame, so a worldspace change with one in flight leaves
            // an entry holding an index into a table that no longer has it — and reading that index
            // is past the end of an emptied span, which is the quietest kind of wrong.
            scene.clear();
            holding.gather(scene, getWorld().getImageManager());
            EXPECT_EQ(holding.getWaitingCount(), 0u) << "a cleared scene left a bake waiting on a material it forgot";
        }
    }
}

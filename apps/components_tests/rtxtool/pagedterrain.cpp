#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <osg/Group>
#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <gtest/gtest.h>

#include <boost/program_options/variables_map.hpp>

#include <components/esm3/loadcell.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/misc/constants.hpp>
#include <components/rtx/compositequeue.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/terraincomposite.hpp>
#include <components/rtx/texturebuilder.hpp>

#include <apps/rtxtool/cellscene.hpp>
#include <apps/rtxtool/world.hpp>

#include "installation.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

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

            readRegion(world, cell, *root, scene, extractor, loaded, /*liveProps=*/false);
            world.setTerrainViewPoint(osg::Vec3f(cell.getGridX() * sCellSize, cell.getGridY() * sCellSize, 0.0f));

            extractor.follow(ask ? world.getTerrainResidency() : nullptr);

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
        TEST(RtxPagedTerrainTest, aPagedWorldsGroundReachesTheMirrorAndOnlyBecauseItWasAskedFor)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            const ESM::Cell* cell = world->findCell(std::string(sOutdoors));
            ASSERT_NE(cell, nullptr);

            world->pageTerrain(true);

            Rtx::SceneDesc asked;
            placeOutdoors(*world, *cell, asked, true);
            ASSERT_NE(world->getTerrainResidency(), nullptr) << "the run did not page its terrain";

            Rtx::SceneDesc unasked;
            placeOutdoors(*world, *cell, unasked, false);

            EXPECT_GT(asked.getPlacedCount(), unasked.getPlacedCount())
                << "the chunks a quad tree hides never reached the mirror";
        }

        /// A world that parents its chunks offers no residency, and is reached by walking.
        ///
        /// **The other half of the same rule**, and what stops the ground being placed twice: a
        /// caller that both walked the graph and asked every terrain world it knew would count
        /// `TerrainGrid`'s chunks once each way.
        TEST(RtxPagedTerrainTest, aGridWorldOffersNoResidencyBecauseItsChunksAreInTheGraph)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            const ESM::Cell* cell = world->findCell(std::string(sOutdoors));
            ASSERT_NE(cell, nullptr);

            Rtx::SceneDesc scene;
            placeOutdoors(*world, *cell, scene, true);

            EXPECT_EQ(world->getTerrainResidency(), nullptr);
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
        TEST(RtxPagedTerrainTest, aPagedWorldStandsStaticsPastTheActiveGridAndAGridWorldStandsNone)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;

            const std::unique_ptr<World> paged = openWorld(config, variables);
            if (paged == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            const ESM::Cell* cell = paged->findCell(std::string(sBuiltUp));
            ASSERT_NE(cell, nullptr);

            const osg::Vec2f middle = middleOf(*cell);

            // Just past the grid, because every chunk wider than a cell is baked on the way in and
            // each bake costs tens of milliseconds. What is under test is that anything at all
            // arrives out there, which one ring of chunks answers as well as ten.
            paged->pageTerrain(true);
            paged->setTerrainViewDistance(2.0f * sCellSize);

            Rtx::SceneDesc withStatics;
            placeOutdoors(*paged, *cell, withStatics, true);
            ASSERT_NE(paged->getTerrainResidency(), nullptr) << "the run did not page its terrain";

            const std::uint32_t stood = standingBeyond(withStatics, middle, sGridReach);
            EXPECT_GT(stood, 0u) << "the distant ground came up bare";

            const std::unique_ptr<World> bare = openWorld(config, variables);
            ASSERT_NE(bare, nullptr);

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

            const std::unique_ptr<World> grid = openWorld(config, variables);
            ASSERT_NE(grid, nullptr);

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
        TEST(RtxPagedTerrainTest, noChunkIsTexturedByARenderTargetHoweverLargeItIs)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            const ESM::Cell* cell = world->findCell(std::string(sOutdoors));
            ASSERT_NE(cell, nullptr);

            world->pageTerrain(true);

            Rtx::SceneDesc grid;
            placeOutdoors(*world, *cell, grid, true);
            const GroundTally near = tallyGround(grid);

            world->setTerrainViewDistance(4.0f * sCellSize);

            Rtx::SceneDesc paged;
            placeOutdoors(*world, *cell, paged, true);
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
        TEST(RtxPagedTerrainTest, aSecondWorldWalkKeepsTheGroundTheGraphDoesNotParent)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            const ESM::Cell* cell = world->findCell(std::string(sOutdoors));
            ASSERT_NE(cell, nullptr);

            world->pageTerrain(true);

            Rtx::SceneDesc once;
            placeOutdoors(*world, *cell, once, true, 1);
            const GroundTally loaded = tallyGround(once);
            ASSERT_GT(loaded.mChunks, 0u) << "no paged ground was placed at all";

            Rtx::SceneDesc again;
            placeOutdoors(*world, *cell, again, true, 3);
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
        /// **And the queue is what bakes it, drained here to the end.** A chunk asks by setting
        /// `Material::mFlatten` and shades from its layer stack until one arrives, which is what
        /// keeps a cell boundary from spending a quarter of a second flattening eight of them: this
        /// asserts both halves, that nothing is flattened before the queue runs and that everything
        /// is after.
        ///
        /// A radius barely past the grid, because every composite in the scene is baked here and each
        /// one costs tens of milliseconds — the figure `plan.md` §6 records.
        TEST(RtxPagedTerrainTest, groundPastACellIsFlattenedIntoOneTextureTheUploaderCanRead)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            const ESM::Cell* cell = world->findCell(std::string(sOutdoors));
            ASSERT_NE(cell, nullptr);

            world->pageTerrain(true);
            world->setTerrainViewDistance(1.25f * sCellSize);

            Rtx::SceneDesc scene;
            placeOutdoors(*world, *cell, scene, true);

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

            // **A second queue that never drains**, so there is still something waiting when the
            // scene is cleared at the end of this test.
            Rtx::CompositeQueue holding;
            holding.gather(scene, world->getImageManager());
            ASSERT_EQ(holding.getWaitingCount(), asked);

            Rtx::CompositeQueue queue;
            queue.gather(scene, world->getImageManager());
            EXPECT_EQ(queue.getWaitingCount(), asked);

            while (queue.getWaitingCount() > 0)
                queue.drain(scene, Rtx::sBakeRowsPerDrain);

            const std::vector<Rtx::Index> flattened = flattenedSlots();
            ASSERT_EQ(flattened.size(), asked) << "the queue drained without finishing what it took on";

            const Rtx::SceneTextures described(scene, world->getImageManager(), &queue);
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

            // **Last, because it empties the scene.** `SceneDesc::clear` renumbers the material
            // table and a bake takes tens of frames, so a worldspace change with one in flight
            // leaves an entry holding an index into a table that no longer has it — and reading
            // that index is past the end of an emptied span, which is the quietest kind of wrong.
            scene.clear();
            holding.gather(scene, world->getImageManager());
            EXPECT_EQ(holding.getWaitingCount(), 0u) << "a cleared scene left a bake waiting on a material it forgot";
        }
    }
}

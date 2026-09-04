#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Group>
#include <osg/Matrixf>

#include <apps/rtxtool/cellscene.hpp>
#include <apps/rtxtool/content.hpp>
#include <apps/rtxtool/stagedworld.hpp>
#include <apps/rtxtool/world.hpp>
#include <components/debug/debugging.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/misc/constants.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/sceneuploader.hpp>

#include "../rtx/countingrenderer.hpp"
#include "installation.hpp"

namespace RtxTool
{
    namespace
    {
        std::ostream& out()
        {
            return Debug::getRawStdout();
        }

        /// Two exterior cells side by side in the middle of a town, so the grid that follows the
        /// camera keeps three columns and swaps three for three.
        constexpr std::string_view sFrom = "-3,-2";
        constexpr std::string_view sTo = "-2,-2";

        /// Far enough from the two above that their rings do not touch, so a region read after one
        /// of them shares no cell with it.
        constexpr std::string_view sAway = "-2,-9";

        /// Brings the ring around `centre` in, takes the ones that left off, and mirrors what is
        /// left — which is what `runWindow`'s `bring` does, in the same order and for the same
        /// reasons.
        std::uint32_t crossTo(World& world, const ESM::Cell& centre, osg::Group& root, Rtx::SceneDesc& scene,
            LoadedCells& loaded, Rtx::SceneExtractor& extractor)
        {
            readRegion(RegionRequest{ world, centre, root, loaded, /*liveProps=*/false });
            const std::uint32_t went = dropCellsOutside(world, centre, root, loaded);

            extractor.extractWorld(root, osg::Matrixf::identity(), 0);
            extractor.advance();

            if (went > 0)
                extractor.retire();

            return went;
        }

        struct RtxCrossingTest : InstallationTest
        {
        };

        /// What a crossing actually costs the renderer, which is the question the harness exists to
        /// put a number on.
        ///
        /// **It appends, and that was not the expected answer.** Walking one cell east drops three
        /// columns as it gains three, and dropping a cell was supposed to compact the tables and
        /// force a full rebuild. It does not, because a town is built out of a few dozen models
        /// placed hundreds of times: the resource cache hands the same nodes to every cell, so the
        /// three columns that left took no mesh with them that the six that stayed were not still
        /// using. `retain` finds every mesh still met, drops nothing, and renumbers nothing — so the
        /// arrival is a growth and `extendScene` is the honest answer to it.
        ///
        /// Measured here at Balmora: 1,397 meshes to 1,665, and 50 textures described where a
        /// rebuild would have decoded and shading-estimated all 231 again.
        TEST_F(RtxCrossingTest, walkingIntoTheNextCellAppendsBecauseATownSharesItsModels)
        {
            const ESM::Cell* from = getContent().findCell(sFrom);
            const ESM::Cell* to = getContent().findCell(sTo);
            ASSERT_NE(from, nullptr);
            ASSERT_NE(to, nullptr);

            const osg::ref_ptr<osg::Group> root = new osg::Group;

            Rtx::SceneDesc scene;
            Rtx::SceneExtractor extractor(scene);
            Rtx::SceneUploader uploader;
            Rtx::Testing::CountingRenderer renderer;
            LoadedCells loaded;

            EXPECT_EQ(crossTo(getWorld(), *from, *root, scene, loaded, extractor), 0u)
                << "nothing was loaded to leave yet";

            // The first ring has nothing to append to, so it builds whatever it found.
            const Rtx::SceneUpload first
                = uploader.hand(renderer, Rtx::sWorld, scene, getWorld().getImageManager(), Rtx::SeaState{});
            ASSERT_EQ(first.mKind, Rtx::SceneUpload::Kind::Rebuilt);
            ASSERT_GT(scene.getPlacedCount(), std::size_t{ 0 }) << "the ring placed no geometry";

            // Standing still is the ordinary frame: the walk finds everything where it was.
            extractor.extract(*root, osg::Matrixf::identity(), 0);
            extractor.advance();
            EXPECT_EQ(uploader.hand(renderer, Rtx::sWorld, scene, getWorld().getImageManager(), Rtx::SeaState{}).mKind,
                Rtx::SceneUpload::Kind::Placed);

            const std::size_t meshesBefore = scene.getMeshes().size();
            const std::size_t texturesBefore = scene.getTextures().size();

            EXPECT_EQ(crossTo(getWorld(), *to, *root, scene, loaded, extractor), 3u)
                << "a step of one cell east leaves the three columns behind it";

            const Rtx::SceneUpload crossed
                = uploader.hand(renderer, Rtx::sWorld, scene, getWorld().getImageManager(), Rtx::SeaState{});
            EXPECT_EQ(crossed.mKind, Rtx::SceneUpload::Kind::Extended);
            EXPECT_GT(scene.getMeshes().size(), meshesBefore) << "three cells arrived and brought no geometry";

            // **Exactly the arrivals, which is the whole saving.** Anything else means the offset
            // the descriptions began at disagreed with where the renderer's array ends, and the
            // difference is every image in the region decoded and shading-estimated a second time.
            EXPECT_EQ(crossed.mDescribed, scene.getTextures().size() - texturesBefore);
            EXPECT_LT(crossed.mDescribed, scene.getTextures().size()) << "the crossing described the whole table";

            // The array ends where the scene's table does, so no material names the wrong image.
            EXPECT_EQ(renderer.mTextures, scene.getTextures().size());
            EXPECT_FALSE(renderer.mAppendedToWrongEnd);
            EXPECT_EQ(renderer.mRebuilt, 1u) << "the crossing cost a full build after all";
        }

        /// The sea is one sheet the world owns, not a footprint per cell.
        ///
        /// **Which is what `MWRender::Water` has always been**: a plane a hundred and fifty cells
        /// across, moved to whichever cell is being looked at. A quad per square was a different
        /// surface from the game's — a different extent, a different tessellation and a different
        /// shoreline — and everything the harness ever judged about caustics, the glitter path or a
        /// grazing Fresnel was judged against it.
        TEST_F(RtxCrossingTest, theSeaIsOneSheetTheWorldOwns)
        {
            const ESM::Cell* from = getContent().findCell(sFrom);
            ASSERT_NE(from, nullptr);

            StagedWorld staged(getWorld(), *from, StagingRequest{}, ActorRequest{});
            ASSERT_FALSE(staged.empty());

            // Balmora is inland and its cells still have water: every exterior does, and the sea is
            // simply below the ground there.
            EXPECT_EQ(staged.getLighting().mWaterLevel, 0.0f) << "an exterior's sea is at zero";

            // **One sheet and one placement of it**, whatever the ring holds. Nine quads is what the
            // footprint-per-cell version left behind, and the count is how the two tell apart.
            std::size_t sheets = 0;
            for (const Rtx::MeshInstance& placed : staged.getScene().getInstances())
            {
                const Rtx::Material& material = staged.getScene().getMaterials()[placed.mMaterial];
                sheets += material.mKind == Rtx::MaterialKind::Water ? 1 : 0;
            }

            EXPECT_EQ(sheets, std::size_t{ 1 }) << "the sea was placed once per cell again";
        }

        /// A region read after another one stands on its own ground, and lights its own flames.
        ///
        /// **Two things outlived the reading that made them**, and `World::clearTerrain` and
        /// `StagedWorld::sSeed` say what each was. Two readings of one region, differing in nothing
        /// but whether another was read between them: nothing about the second is a function of the
        /// first, so the two come to one answer.
        TEST_F(RtxCrossingTest, aRegionReadAfterAnotherStandsOnItsOwnGround)
        {
            const ESM::Cell* away = getContent().findCell(sAway);
            const ESM::Cell* town = getContent().findCell(sFrom);
            ASSERT_NE(away, nullptr);
            ASSERT_NE(town, nullptr);

            // **Nobody standing in it and its emitters running**, which is the pair of answers under
            // test: the ground comes from the terrain, and the flames from a generator the process
            // shares. Residents would only add a third thing to explain a count with.
            const ActorRequest props{ .mResidents = false, .mProps = true };

            struct Reading
            {
                std::uint32_t mPlaced = 0;

                /// Every sprite's position, added up without their signs.
                ///
                /// **A count is the wrong oracle for an emitter**: a birth rate the generator did
                /// not move emits the same number of particles from somewhere else. Absolute,
                /// because a plume is roughly symmetric about its own emitter and the signed sum of
                /// one is most of the way to nought.
                double mSpriteSum = 0.0;
            };

            const auto read = [&](const ESM::Cell& cell) {
                StagedWorld staged(getWorld(), cell, StagingRequest{}, props);

                Reading held{ .mPlaced = staged.getScene().getPlacedCount() };
                for (const Rtx::Sprite& sprite : staged.getScene().getSprites())
                {
                    const float sum = std::abs(sprite.mPosition.x()) + std::abs(sprite.mPosition.y())
                        + std::abs(sprite.mPosition.z());
                    held.mSpriteSum += static_cast<double>(sum);
                }

                return held;
            };

            const Reading alone = read(*away);
            ASSERT_GT(alone.mPlaced, 0u) << "the region placed nothing, so this proves nothing";
            ASSERT_GT(alone.mSpriteSum, 0.0) << "the region emitted nothing, so this proves half of it";

            read(*town);

            const Reading after = read(*away);
            EXPECT_EQ(after.mPlaced, alone.mPlaced) << "the region stood on the ground of the one read before it";
            EXPECT_EQ(after.mSpriteSum, alone.mSpriteSum)
                << "the emitters carried on from where the one before left off";
        }

        /// Walking every frame leaves the scene exactly where it was.
        ///
        /// **The cadence the game runs at, which is now the only cadence this has.** A still world
        /// used to be kept as a snapshot between cell crossings, so anything a sweep emptied stayed
        /// empty until the ring next moved — and the game, which re-walks and re-sweeps every frame,
        /// could never have hidden that.
        ///
        /// A walk that leaves the scene where it was is the whole property: emptied and refilled on
        /// the same cadence, so a light counted twice per walk or one nothing put back both show up
        /// as a count that moves.
        ///
        /// **Each of those walks ends in a sweep, and the geometry count is what says it takes
        /// nothing.** A sweep on a still frame that freed a live mesh leaves a hole nothing refills,
        /// because nothing arrived to fill it.
        TEST_F(RtxCrossingTest, walkingEveryFrameLeavesTheSceneWhereItWas)
        {
            const ESM::Cell* from = getContent().findCell(sFrom);
            ASSERT_NE(from, nullptr);

            // **Nobody in it, which is the case under test.** A region with residents or live props
            // already walks every frame through them, so a default request would have exercised
            // their stepping and reported that this worked.
            const ActorRequest empty{ .mResidents = false, .mProps = false };

            StagedWorld staged(getWorld(), *from, StagingRequest{}, empty);
            ASSERT_FALSE(staged.empty());
            ASSERT_EQ(staged.getActorCount(), std::size_t{ 0 });
            ASSERT_NE(staged.getMotion(), nullptr) << "a still world is walked every frame too";

            // A freed slot keeps its index and its room, so the table's own size never falls and
            // only the entries that still describe geometry can say what the sweep took.
            const auto liveMeshes = [&] {
                std::size_t found = 0;
                for (const Rtx::MeshRange& mesh : staged.getScene().getMeshes())
                    if (mesh.mVertexCount > 0)
                        ++found;
                return found;
            };

            const std::size_t lights = staged.getScene().getLights().size();
            const std::size_t placed = staged.getScene().getPlacedCount();
            const std::size_t meshes = liveMeshes();
            ASSERT_GT(lights, std::size_t{ 0 }) << "nothing to notice going missing";
            ASSERT_GT(meshes, std::size_t{ 0 }) << "nothing for a sweep to take";

            for (std::uint32_t frame = 1; frame <= 4; ++frame)
            {
                EXPECT_TRUE(staged.getMotion()->step(frame)) << "a walk always has to be handed over";
                EXPECT_EQ(staged.getScene().getLights().size(), lights) << "at frame " << frame;
                EXPECT_EQ(staged.getScene().getPlacedCount(), placed) << "at frame " << frame;
                EXPECT_EQ(liveMeshes(), meshes) << "the frame's own sweep took geometry at frame " << frame;
            }
        }

        /// The ground a quad tree hides is still there on the frames after the one that placed it.
        ///
        /// **The sibling above walks a region with nobody in it, and that is why this was missed.**
        /// A world with residents steps through their own stepper instead, and that walked the same
        /// root without asking for what the graph does not parent — so the sweep after it retired
        /// every chunk a paged world had handed over. The ground reached the mirror on the first
        /// frame and was gone by the second, which reads as a town standing on open sea while the
        /// scene, the top level and the instance count all look right.
        ///
        /// Residents on, paged terrain on: the two conditions together are the bug.
        TEST_F(RtxCrossingTest, aPagedWorldsGroundSurvivesTheFramesAfterIt)
        {
            const ESM::Cell* from = getContent().findCell(std::string(sFrom));
            ASSERT_NE(from, nullptr);

            getWorld().pageTerrain(true);

            StagedWorld staged(getWorld(), *from, StagingRequest{}, ActorRequest{});
            ASSERT_FALSE(staged.empty());
            ASSERT_GT(staged.getActorCount(), std::size_t{ 0 }) << "no residents, so the stepper under test never runs";
            ASSERT_NE(staged.getMotion(), nullptr);

            const auto chunks = [&] {
                const Rtx::SceneDesc& scene = staged.getScene();
                std::size_t found = 0;
                for (const Rtx::MeshInstance& instance : scene.getInstances())
                {
                    if (!instance.isPlaced() || instance.mMaterial == Rtx::sNoIndex)
                        continue;
                    if (scene.getMaterials()[instance.mMaterial].mKind == Rtx::MaterialKind::Terrain)
                        ++found;
                }
                return found;
            };

            const std::size_t placed = chunks();
            ASSERT_GT(placed, std::size_t{ 0 }) << "no paged ground was placed at all";

            for (std::uint32_t frame = 1; frame <= 4; ++frame)
            {
                EXPECT_TRUE(staged.getMotion()->step(frame));
                EXPECT_EQ(chunks(), placed) << "the ground a quad tree hides was swept at frame " << frame;
            }
        }

        /// The people who stood in a cell leave with it.
        ///
        /// **An actor used to hang on the run's own root.** Everything else a cell brings is under a
        /// group, so taking that node off the root is the whole of unloading — and the residents,
        /// parented beside it rather than under it, stayed posed and placed in a street that was no
        /// longer there. They now hang under the cell that placed them and go when it goes.
        ///
        /// This counts them. That their geometry went with them is the parenting itself, which no
        /// count can see: a record dropped while the node stays hung on the root leaves a creature
        /// standing with nothing owning it, and both halves are needed.
        TEST_F(RtxCrossingTest, theResidentsOfACellLeaveWithIt)
        {
            const ESM::Cell* from = getContent().findCell(std::string(sFrom));
            ASSERT_NE(from, nullptr);

            StagedWorld staged(getWorld(), *from, StagingRequest{}, ActorRequest{});
            const std::size_t standing = staged.getActorCount();
            ASSERT_GT(standing, std::size_t{ 0 }) << "a town with nobody in it cannot show anyone leaving";

            // Far enough that every cell the run started in is behind the ring: a step of one column
            // keeps two of the three, and what is under test is a cell actually going.
            const auto side = static_cast<float>(Constants::CellSizeInUnits);
            const osg::Vec3f away((static_cast<float>(from->getGridX()) + 4.5f) * side,
                (static_cast<float>(from->getGridY()) + 0.5f) * side, 0.0f);

            ASSERT_GT(staged.moveTo(away).mDeparted, std::uint32_t{ 0 }) << "nothing was unloaded";
            EXPECT_LT(staged.getActorCount(), standing) << "the residents outlived the cell that placed them";
        }

        /// The lamps are still burning after the crossing that swept the scene.
        ///
        /// **The bug this is here for, in one line.** A camera stepped out of the square it started
        /// in, three cells died, `SceneDesc::release` emptied the light table on the way past — and
        /// every lamp read out of a `LIGH` record went out and stayed out, because the walk that was
        /// supposed to refill the table had never carried them in the first place.
        TEST_F(RtxCrossingTest, aCrossingLeavesTheLampsBurning)
        {
            const ESM::Cell* from = getContent().findCell(sFrom);
            ASSERT_NE(from, nullptr);

            StagedWorld staged(getWorld(), *from, StagingRequest{}, ActorRequest{});
            ASSERT_FALSE(staged.empty());

            const std::size_t before = staged.getScene().getLights().size();
            ASSERT_GT(before, std::size_t{ 0 }) << "Balmora's nine cells cast no light to begin with";

            // The middle of the square east of this one, which is a crossing and not a step.
            constexpr float side = 8192.0f;
            const Crossing crossed
                = staged.moveTo(osg::Vec3f(-2.0f * side + 0.5f * side, -2.0f * side + 0.5f * side, 0.0f));
            ASSERT_GT(crossed.mDeparted, std::uint32_t{ 0 }) << "nothing left, so nothing swept";

            EXPECT_GT(staged.getScene().getLights().size(), std::size_t{ 0 })
                << "the sweep took the lamps and no walk put them back";
        }

        /// Walking a long way leaves a working set the size of the grid, not the size of the walk.
        ///
        /// **The instrument for the streaming bench.** A camera flown across the island crosses
        /// twenty boundaries, and if each one only adds then what is being measured after the fifth
        /// is a world no player ever holds. This walks a straight line of cells and asserts the
        /// count settles instead of climbing.
        TEST_F(RtxCrossingTest, walkingAcrossManyCellsHoldsAGridRatherThanEverythingVisited)
        {
            const osg::ref_ptr<osg::Group> root = new osg::Group;

            Rtx::SceneDesc scene;
            Rtx::SceneExtractor extractor(scene);
            LoadedCells loaded;

            std::size_t afterFirstStep = 0;
            std::size_t placed = 0;

            // North out of Balmora, one cell at a time, over land the whole way.
            for (int y = -2; y <= 6; ++y)
            {
                const ESM::Cell* cell = getContent().findCell("-3," + std::to_string(y));
                ASSERT_NE(cell, nullptr) << "no cell at -3," << y;

                crossTo(getWorld(), *cell, *root, scene, loaded, extractor);
                placed = scene.getPlacedCount();

                if (y == -1)
                    afterFirstStep = placed;

                out() << "  at -3," << y << ": " << placed << " placed, " << scene.getMeshes().size() << " meshes\n";
            }

            ASSERT_GT(afterFirstStep, std::size_t{ 0 });

            // **Twice the first step's grid, and the room is for content and not for a leak.** Nine
            // cells of ashland hold less than nine of town, so the count moves with where the camera
            // is; what it must not do is carry the eight grids it has already left.
            EXPECT_LT(placed, afterFirstStep * 2)
                << "the working set grew with the walk rather than with what is around the camera";
        }
    }
}

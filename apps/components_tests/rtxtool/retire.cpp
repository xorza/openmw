#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Group>

#include <apps/rtxtool/cellscene.hpp>
#include <apps/rtxtool/content.hpp>
#include <apps/rtxtool/lighting.hpp>
#include <apps/rtxtool/placement.hpp>
#include <apps/rtxtool/world.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/rtx/camera.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/texturebuilder.hpp>

#include "../rtx/harness.hpp"
#include "installation.hpp"

namespace RtxTool
{
    namespace
    {
        /// Two interiors far enough apart in the content to share little and near enough in kind to
        /// share something: both are Imperial buildings in the same town, so the same barrels,
        /// tables and lamps stand in each.
        constexpr std::string_view sFirst = "Seyda Neen, Census and Excise Office";
        constexpr std::string_view sSecond = "Seyda Neen, Arrille's Tradehouse";

        /// Builds one room's graph and mirrors it, keeping the graph alive in `kept`.
        ///
        /// **A root per room, and the caller holds them.** Walking a second room's root does not
        /// stamp the first one's placements, which is exactly how a cell goes: the sweep finds them
        /// unmet. And the roots have to outlive the mirror, because it keys its meshes on the nodes
        /// in them and a freed address is one the allocator can hand back holding something else.
        Rtx::ExtractionStats readCell(World& world, const ESM::Cell& cell, Rtx::SceneDesc& scene,
            std::vector<osg::ref_ptr<osg::Group>>& kept, Rtx::SceneExtractor& extractor)
        {
            osg::ref_ptr<osg::Group> root = new osg::Group;
            kept.push_back(root);

            LoadedCells loaded;
            readRegion(RegionRequest{ world, cell, *root, loaded, /*liveProps=*/false });
            return extractor.extract(*root, osg::Matrixf::identity(), 0);
        }

        /// A fresh root, kept alive by the caller for as long as the mirror names its nodes.
        osg::ref_ptr<osg::Group> keepRoot(std::vector<osg::ref_ptr<osg::Group>>& kept)
        {
            kept.push_back(new osg::Group);
            return kept.back();
        }

        /// What one room's load left standing.
        struct Room
        {
            osg::ref_ptr<osg::Group> mRoot;
            CellLighting mLighting;
        };

        /// Builds one room into its own root, mirrors it, and hands back the graph and what lit it.
        Room loadAndMirror(World& world, const ESM::Cell& cell, std::vector<osg::ref_ptr<osg::Group>>& kept,
            Rtx::SceneDesc& scene, Rtx::SceneExtractor& extractor)
        {
            const osg::ref_ptr<osg::Group> root = keepRoot(kept);

            LoadedCells loaded;
            const CellLighting lit = loadRegion(
                RegionRequest{ world, cell, *root, loaded, false }, scene, extractor, SkyMoment{ "Clear", 0, 12.0f })
                                         .mLighting;
            extractor.extract(*root, osg::Matrixf::identity(), 0);

            return Room{ .mRoot = root, .mLighting = lit };
        }

        /// One number per mesh, over the vertices and the triangles it actually holds.
        ///
        /// Sorted into a multiset by the caller, this says whether two scenes are made of the same
        /// geometry whatever order they put it in — which is the one thing a compaction can get
        /// wrong and an offset check cannot see: a range that moved while its offset did not leaves
        /// every count consistent and every mesh wearing its neighbour's triangles.
        std::multiset<std::uint64_t> meshFingerprints(const Rtx::SceneDesc& scene)
        {
            std::multiset<std::uint64_t> prints;
            for (Rtx::Index mesh = 0; mesh < scene.getMeshes().size(); ++mesh)
            {
                // A freed slot describes nothing and is not a mesh to compare; it keeps its room
                // until something fits into it.
                if (scene.getMeshes()[mesh].mVertexCount == 0)
                    continue;

                std::uint64_t print = 0xcbf29ce484222325ull;
                const auto fold = [&](std::uint32_t word) { print = (print ^ word) * 0x100000001b3ull; };

                for (const osg::Vec3f& vertex : scene.getMeshPositions(mesh))
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        std::uint32_t bits = 0;
                        std::memcpy(&bits, vertex.ptr() + axis, sizeof(bits));
                        fold(bits);
                    }

                for (const std::uint32_t index : scene.getMeshIndices(mesh))
                    fold(index);

                prints.insert(print);
            }

            return prints;
        }

        struct RtxRetireTest : InstallationTest
        {
        };

        /// A cell the walk stopped finding leaves the scene, and what the next cell shares with it
        /// stays exactly where it was.
        ///
        /// **Two rooms out of the shipped content, because the interesting case cannot be built by
        /// hand.** A synthetic graph either overlaps completely or not at all; two Imperial
        /// interiors share their barrels and their lamps and nothing else, so the sweep has to drop
        /// one room's geometry while the shared models — met again in the second walk, through the
        /// very pointers the resource cache handed out for the first — carry through the compaction
        /// and go on resolving.
        TEST_F(RtxRetireTest, aCellTheWalkStoppedFindingLeavesAndWhatIsSharedStays)
        {
            const ESM::Cell* first = getContent().findCell(std::string(sFirst));
            const ESM::Cell* second = getContent().findCell(std::string(sSecond));
            ASSERT_NE(first, nullptr);
            ASSERT_NE(second, nullptr);

            std::vector<osg::ref_ptr<osg::Group>> kept;
            Rtx::SceneDesc scene;
            Rtx::SceneExtractor extractor(scene);

            const Rtx::ExtractionStats one = readCell(getWorld(), *first, scene, kept, extractor);
            ASSERT_GT(one.mMeshesAdded, 0u);

            const std::size_t held = scene.getMeshes().size();
            const std::uint64_t built = scene.getStructureRevision();

            // Nothing has gone: the walk that just happened is the epoch everything survives.
            ASSERT_TRUE(extractor.retire().empty());
            EXPECT_EQ(scene.getStructureRevision(), built) << "a sweep that dropped nothing must not ask for a rebuild";
            EXPECT_EQ(scene.getMeshes().size(), held);

            // The second room, and only the second room. The first is still in the resource cache,
            // so its drawables are alive and would be recognised if anything walked them.
            scene.clearPlacement();
            const Rtx::ExtractionStats two = readCell(getWorld(), *second, scene, kept, extractor);

            ASSERT_GT(two.mMeshesReused, 0u) << "two Imperial interiors that share no model at all";
            ASSERT_GT(two.mMeshesAdded, 0u);
            ASSERT_EQ(scene.getMeshes().size(), held + two.mMeshesAdded);

            const std::size_t verticesBefore = scene.getPositions().size();

            // Taken here and not before the second room arrived: that walk added meshes, which is a
            // structure change and rightly one. What must not change is the *sweep*.
            const std::uint64_t beforeSweep = scene.getStructureRevision();
            const Rtx::Retirement went = extractor.retire();

            EXPECT_GT(went.mMeshes, 0u) << "the room that was walked away from is still in the scene";
            EXPECT_LT(went.mMeshes, held) << "the models the two rooms share were dropped along with it";

            // **The headline, and the whole reason the tables stopped compacting.** A cell leaving
            // is not a structure change: nothing arrived and nothing moved, so every bottom-level
            // acceleration structure in the scene is still correct and the frame after a boundary
            // costs the top level and nothing else. This used to be the assertion that it *had*
            // changed, and that was nineteen full rebuilds on a route across the island.
            EXPECT_EQ(scene.getStructureRevision(), beforeSweep) << "a cell leaving asked for a rebuild";

            // Nothing shrinks. The freed slots keep their room and wait for something that fits.
            EXPECT_EQ(scene.getMeshes().size(), held + two.mMeshesAdded);
            EXPECT_EQ(scene.getPositions().size(), verticesBefore);

            std::size_t freed = 0;
            for (const Rtx::MeshRange& range : scene.getMeshes())
                freed += range.mVertexCount == 0 ? 1 : 0;

            EXPECT_EQ(freed, went.mMeshes) << "a slot was emptied without being reported, or the other way round";

            // Every surviving offset still describes its own mesh. This was the compaction's chance
            // to get it wrong; now it is the chance a reused slot has, which is the same check.
            for (Rtx::Index mesh = 0; mesh < scene.getMeshes().size(); ++mesh)
            {
                const Rtx::MeshRange& range = scene.getMeshes()[mesh];
                ASSERT_LE(std::size_t{ range.mVertexOffset } + range.mVertexCount, scene.getPositions().size());
                ASSERT_LE(std::size_t{ range.mIndexOffset } + range.mIndexCount, scene.getIndices().size());

                for (const std::uint32_t index : scene.getMeshIndices(mesh))
                    ASSERT_LT(index, range.mVertexCount) << "mesh " << mesh << " indexes past its own vertices";
            }

            // **A path is empty exactly where the slot is free**, and the two have to agree: a live
            // texture that lost its path is a surface with nothing on it, and a freed one that kept
            // its path is a lookup that hands out a slot nothing stands in.
            for (Rtx::Index slot = 0; slot < scene.getTextures().size(); ++slot)
            {
                const VFS::Path::Normalized& path = scene.getTextures()[slot];
                if (path.value().empty())
                    continue;

                EXPECT_EQ(scene.addTexture(path), slot) << "a live texture is not findable by its own path";
            }

            for (const Rtx::Material& material : scene.getMaterials())
            {
                if (material.mDiffuse != Rtx::sNoIndex)
                {
                    EXPECT_LT(material.mDiffuse, scene.getTextures().size());
                }

                EXPECT_LE(std::size_t{ material.mLayerOffset } + material.mLayerCount, scene.getLayers().size());
            }

            // **The same geometry a scene that had never heard of the first room would hold**, once
            // the empty slots are passed over. Two scenes agreeing mesh for mesh on what is in them,
            // whatever order they arrived in and whatever gaps one of them is carrying.
            Rtx::SceneDesc alone;
            {
                Rtx::SceneExtractor fresh(alone);
                readCell(getWorld(), *second, alone, kept, fresh);
            }

            EXPECT_EQ(meshFingerprints(scene), meshFingerprints(alone));

            // And the survivors go on resolving through the identity map: a third walk of the same
            // room adds nothing at all, because nothing moved under it.
            scene.clearPlacement();
            const Rtx::ExtractionStats again = readCell(getWorld(), *second, scene, kept, extractor);

            EXPECT_EQ(again.mMeshesAdded, 0u) << "a survivor was not recognised after the sweep";
            EXPECT_EQ(again.mMaterialsAdded, 0u);
            EXPECT_EQ(again.mMeshesReused, two.mMeshesReused + two.mMeshesAdded)
                << "the same room, and every drawable in it resolving to something already here";

            // **Walking back into the first room reuses what it left behind.** That is what the
            // allocator is for: the geometry buffers hold their high-water mark rather than the sum
            // of every room ever entered.
            extractor.retire();
            scene.clearPlacement();
            readCell(getWorld(), *first, scene, kept, extractor);

            // A percent of slack, and it is external fragmentation rather than a leak: best fit
            // puts a run in the smallest hole that holds it and leaves the remainder, and a
            // remainder too short for the next run is room nothing can use until its neighbours
            // come back. Measured at forty vertices in thirty thousand on this pair of rooms. What
            // it must not do is append the room again, which is thousands and not tens.
            EXPECT_LE(scene.getPositions().size(), verticesBefore + verticesBefore / 100)
                << "the room came back and the buffers grew instead of taking the room it left";
        }

        /// The renderer builds the same picture out of a compacted scene as out of one that never
        /// lost anything.
        ///
        /// **What the checks above cannot reach.** They say the two scenes hold the same geometry;
        /// this says a bottom-level structure built over a moved range still describes the mesh
        /// whose offset names it. An offset that moved by one vertex passes every count and comes
        /// out as a room made of somebody else's triangles.
        ///
        /// The two scenes name their meshes and their textures in different orders — one is what a
        /// compaction left and the other what a walk produced — so a picture that matches is the
        /// indices agreeing all the way through the build and not the tables happening to be equal.
        TEST_F(RtxRetireTest, aCompactedSceneRendersAsOneThatNeverLostAnything)
        {
            // **Asked for before the world is walked**, so a machine with no device skips in a
            // millisecond rather than after two rooms of it.
            std::string reason;
            Rtx::Renderer* renderer = Rtx::Testing::getRenderer(reason);
            if (renderer == nullptr)
                GTEST_SKIP() << reason;

            const ESM::Cell* first = getContent().findCell(std::string(sFirst));
            const ESM::Cell* second = getContent().findCell(std::string(sSecond));
            ASSERT_NE(first, nullptr);
            ASSERT_NE(second, nullptr);

            // The sequence the game runs: a walk, a sweep, a walk of somewhere else, a sweep that
            // drops what the first walk had — and then the walk that puts the placements back,
            // because compacting takes them with it.
            std::vector<osg::ref_ptr<osg::Group>> kept;
            Rtx::SceneDesc scene;
            Rtx::SceneExtractor extractor(scene);
            loadAndMirror(getWorld(), *first, kept, scene, extractor);

            ASSERT_TRUE(extractor.retire().empty());

            Room room;
            for (int pass = 0; pass < 2; ++pass)
            {
                scene.clearPlacement();
                room = loadAndMirror(getWorld(), *second, kept, scene, extractor);

                if (pass == 0)
                {
                    EXPECT_FALSE(extractor.retire().empty()) << "the first room is still in the scene";
                }
            }

            // **The same graph walked into a scene of its own, and not the room loaded a third
            // time.** A lamp's flicker phase is derived from its `SceneUtil::LightSource` id and the
            // ids are one counter for the process, so another load lights the same room at another
            // point of the same flame — which is a difference in the light where this is measuring a
            // difference in the geometry. What the two scenes do not share is what they are here
            // for: one names its meshes and textures in the order a compaction left them, the other
            // in the order a walk produced.
            Rtx::SceneDesc alone;
            Rtx::SceneExtractor fresh(alone);
            fresh.extract(*room.mRoot, osg::Matrixf::identity(), 0);

            // The last walk put the second room back; this is the sweep that takes the first one's
            // placements with it, and without it the scene is still holding both.
            extractor.retire();

            // By what is placed and not by how many slots exist: a compacted scene has gaps where a
            // fresh one has none, and a gap is a name nothing is standing in rather than a placement.
            ASSERT_EQ(scene.getPlacedCount(), alone.getPlacedCount());

            // **The binary's renderer rather than one of this test's own**, which nothing here wants:
            // the only thing it needs that the shared one does not carry is an extent, and a resize
            // costs five milliseconds. `Rtx::Testing::getRenderer` says what a second one costs.
            renderer->resize(640, 360);
            const Rtx::FrameExtents extents = renderer->getExtents();

            // **One camera and one lighting for both.** The two scenes hold the same geometry and are
            // the same room, so the same view of it under the same lamps is what makes the pictures
            // comparable at all. The camera is derived from the scene that never lost anything.
            const Placement placement = placeCamera(alone.getBounds(), 60.0f, std::nullopt, std::nullopt);

            const auto draw = [&](const Rtx::SceneDesc& drawn, std::vector<std::uint8_t>& out) {
                Rtx::SceneTextures described;
                described.describeAll(drawn, getWorld().getImageManager());
                renderer->setScene(Rtx::sWorld, drawn, described.getDescriptions(), Rtx::SeaState{});

                Rtx::Shaders::VisibilityConstants camera = Rtx::makeCamera(placement.mOrigin, placement.mTarget, 60.0f,
                    extents.mRenderWidth, extents.mRenderHeight, 100000.0f);
                applyLighting(room.mLighting, camera);

                // Held rather than measured: an exposure taken off the frame turns any difference at
                // all into a difference everywhere, which is a worse instrument than the pixels.
                renderer->renderFrame(camera, Rtx::FrameOptions{ .mExposure = 1.0f });
                const Rtx::FrameResult result = renderer->finishFrame().value();
                renderer->readPixels(out);
                return result.mHits;
            };

            std::vector<std::uint8_t> compacted;
            std::vector<std::uint8_t> whole;
            const std::uint32_t hitsCompacted = draw(scene, compacted);
            const std::uint32_t hitsWhole = draw(alone, whole);

            ASSERT_GT(hitsWhole, 0u) << "the camera faced away from the room";
            EXPECT_EQ(hitsCompacted, hitsWhole);
            ASSERT_EQ(compacted.size(), whole.size());

            // **To the byte, and there is no tolerance here to hide behind.** A compaction reorders
            // the bottom-level structures and the tables that name them, and the trace comes out of
            // the far side with the same 921,600 bytes: reaching a pixel's candidates in another
            // order is not the same as reaching other candidates. A mesh built over the wrong range
            // shows up as a wall in the wrong place, which is thousands of bytes and not one.
            std::size_t differing = 0;
            for (std::size_t at = 0; at < compacted.size(); ++at)
                differing += compacted[at] != whole[at] ? 1 : 0;

            EXPECT_EQ(differing, std::size_t{ 0 }) << "the compacted scene drew a different picture";
        }

    }
}

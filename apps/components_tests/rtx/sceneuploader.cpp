#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/refusal.hpp>
#include <components/rtx/refusals.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneuploader.hpp>
#include <components/rtx/slot.hpp>
#include <components/vfs/pathutil.hpp>

#include "support/countingrenderer.hpp"
#include "support/geometry.hpp"

namespace Rtx
{
    namespace
    {
        /// The three branches in the order a session takes them, each proved by what the renderer
        /// was asked to do and by how much describing it cost.
        TEST(RtxSceneUploaderTest, aSessionTakesEachBranchInTheOrderItReachesThem)
        {

            Rtx::SceneDesc scene;
            SceneUploader uploader;
            Testing::CountingRenderer renderer;

            const Testing::Model first = Testing::addModel(scene, VFS::Path::NormalizedView("textures/one.dds"));
            const Testing::Model second = Testing::addModel(scene, VFS::Path::NormalizedView("textures/two.dds"));

            // **First time through there is nothing to append to**, so two textures arriving is a
            // build of everything even though nothing was renumbered.
            const SceneUpload built = uploader.hand(
                renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = scene });
            EXPECT_EQ(built.mKind, SceneUpload::Kind::Rebuilt);
            EXPECT_EQ(built.mDescribed, std::size_t{ 2 });
            EXPECT_EQ(scene.refusals().count(Refused::Texture), 2u)
                << "a path that names nothing is described as the stand-in, and refused";
            EXPECT_EQ(renderer.mRebuilt, 1u);
            EXPECT_EQ(renderer.mTextures, 2u);

            // Nothing has changed, which is every ordinary frame: the transforms are rewritten and
            // not one texture is looked at again.
            const SceneUpload still = uploader.hand(
                renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = scene });
            EXPECT_EQ(still.mKind, SceneUpload::Kind::Placed);
            EXPECT_EQ(still.mDescribed, std::size_t{ 0 });
            EXPECT_EQ(renderer.mPlaced, 1u);
            EXPECT_EQ(renderer.mRebuilt, 1u) << "an unchanged scene must not cost a rebuild";

            // A ring arrives: a third model, so the tables grew and nothing moved.
            const Testing::Model third = Testing::addModel(scene, VFS::Path::NormalizedView("textures/three.dds"));

            const SceneUpload grown = uploader.hand(
                renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = scene });
            EXPECT_EQ(grown.mKind, SceneUpload::Kind::Extended);
            EXPECT_EQ(grown.mDescribed, std::size_t{ 1 }) << "only the arrival is described, not the table";
            EXPECT_EQ(renderer.mExtended, 1u);
            EXPECT_EQ(renderer.mRebuilt, 1u);
            EXPECT_EQ(renderer.mTextures, 3u);
            EXPECT_FALSE(renderer.mAppendedToWrongEnd) << "the arrivals began somewhere other than the array's end";

            // The first model goes, in the order a sweep goes in: its placement first, then the
            // tables. **Nothing is renumbered by that any more**, so it is not a rebuild — the frame
            // after a cell leaves costs the top level and nothing else.
            scene.placements().drop(first.mPlacement, Stander::Walk);

            const Rtx::Index keptMeshes[2] = { second.mMesh, third.mMesh };
            const Rtx::Index keptMaterials[2] = { second.mMaterial, third.mMaterial };
            ASSERT_TRUE(scene.release(keptMeshes, keptMaterials));

            const SceneUpload left = uploader.hand(
                renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = scene });
            EXPECT_EQ(left.mKind, SceneUpload::Kind::Placed) << "a cell leaving cost a build";
            EXPECT_EQ(renderer.mRebuilt, 1u);
            EXPECT_EQ(renderer.mExtended, 1u);
            EXPECT_EQ(renderer.mTextures, 3u);

            // **And its texture is given back on that same frame.** Nothing arrived, so this is the
            // branch that used to do only the placing — and waiting for an arrival to hand the memory
            // over is a whole grid of cells on a route that keeps moving.
            EXPECT_EQ(left.mDropped, std::size_t{ 1 });
            EXPECT_EQ(renderer.mDropped, (std::vector<std::uint32_t>{ first.mTexture }));
            EXPECT_EQ(renderer.mTextures, 3u) << "the array shrank, so an append would begin in the wrong place";

            // Named once and then forgotten, which is what the arrivals being cleared on this branch
            // buys: a slot dropped every frame until something took it over would be a drop per
            // frame for as long as the region was gone.
            EXPECT_EQ(
                uploader
                    .hand(renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = scene })
                    .mDropped,
                std::size_t{ 0 });
            EXPECT_EQ(renderer.mDropped.size(), std::size_t{ 1 });

            // **A ring arriving into the slot one left is still an append.** Nothing renumbered, so
            // the rebuild that opened this test stays the only one — which is the whole of what an
            // incremental mirror is worth.
            const Testing::Model fourth = Testing::addModel(scene, VFS::Path::NormalizedView("textures/four.dds"));

            const SceneUpload grew = uploader.hand(
                renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = scene });
            EXPECT_EQ(grew.mKind, SceneUpload::Kind::Extended);
            EXPECT_EQ(grew.mDescribed, std::size_t{ 1 }) << "the arrival, and not the whole table";
            EXPECT_EQ(renderer.mRebuilt, 1u) << "nothing renumbered, so nothing was built again";

            // **The loader is the uploader's own and outlives every scene it describes**, so what
            // it hands over here has to be this frame's arrival and nothing held from the frames
            // behind it. Four hand-overs stand behind this one.
            EXPECT_EQ(renderer.mDescribedSlots, (std::vector<std::uint32_t>{ fourth.mTexture }))
                << "the loader answered with what it held from an earlier frame";

            // **A crossing, which is the two at once**: one ring arrives as another goes, on one
            // frame. Both lists are applied and neither costs a rebuild.
            const Testing::Model fifth = Testing::addModel(scene, VFS::Path::NormalizedView("textures/five.dds"));
            scene.placements().drop(fourth.mPlacement, Stander::Walk);

            const Rtx::Index stillHere[3] = { second.mMesh, third.mMesh, fifth.mMesh };
            const Rtx::Index stillWorn[3] = { second.mMaterial, third.mMaterial, fifth.mMaterial };
            ASSERT_TRUE(scene.release(stillHere, stillWorn));

            const SceneUpload crossed = uploader.hand(
                renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = scene });
            EXPECT_EQ(crossed.mKind, SceneUpload::Kind::Extended);
            EXPECT_EQ(crossed.mDescribed, std::size_t{ 1 }) << "the arrival, and not the one that went";
            EXPECT_EQ(crossed.mDropped, std::size_t{ 1 });
            EXPECT_EQ(renderer.mDropped.back(), fourth.mTexture);
            EXPECT_FALSE(renderer.mAppendedToWrongEnd);
        }

        /// What the device could not stand is reported with what the describe could not, once a
        /// build or an arrival has asked it and never on a placement, which stands nothing.
        ///
        /// **The describe refuses the path nothing reads, and the device refuses a mesh and a
        /// texture of its own**: two textures and a mesh, each named once. A placement asks the
        /// renderer nothing, and a refusal the device repeats at the next arrival is the one already
        /// named, so only the arrival's own unreadable texture is new.
        TEST(RtxSceneUploaderTest, whatTheDeviceCouldNotStandIsReportedWithTheScenesRefusals)
        {

            Rtx::SceneDesc scene;
            SceneUploader uploader;
            Testing::CountingRenderer renderer;
            renderer.mRefusing = {
                Refusal{ .mKind = Refused::Mesh, .mWhy = "no device memory is left for it" },
                Refusal{ .mKind = Refused::Texture,
                    .mName = "textures/vast.dds",
                    .mWhy = "no device memory is left for it" },
            };

            Testing::addModel(scene, VFS::Path::NormalizedView("textures/one.dds"));
            const auto hand = [&] {
                return uploader.hand(
                    renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = scene });
            };

            EXPECT_EQ(hand().mKind, SceneUpload::Kind::Rebuilt);
            EXPECT_EQ(scene.refusals().count(Refused::Mesh), 1u);
            EXPECT_EQ(scene.refusals().count(Refused::Texture), 2u);

            renderer.mRefusing.push_back(Refusal{ .mKind = Refused::Mesh, .mWhy = "a placement asked" });
            EXPECT_EQ(hand().mKind, SceneUpload::Kind::Placed);
            EXPECT_EQ(scene.refusals().count(Refused::Mesh), 1u) << "a placement asked the device what it refused";
            renderer.mRefusing.pop_back();

            Testing::addModel(scene, VFS::Path::NormalizedView("textures/two.dds"));
            EXPECT_EQ(hand().mKind, SceneUpload::Kind::Extended);
            EXPECT_EQ(scene.refusals().count(Refused::Mesh), 1u);
            EXPECT_EQ(scene.refusals().count(Refused::Texture), 3u);
        }

        /// Two uploaders over one scene do not share a decision, which is what makes one per renderer
        /// the rule rather than a habit.
        TEST(RtxSceneUploaderTest, anUploaderThatHasBuiltNothingRebuildsASceneTheOtherOnlyPlaces)
        {

            Rtx::SceneDesc scene;
            Testing::addModel(scene, VFS::Path::NormalizedView("textures/one.dds"));

            SceneUploader built;
            Testing::CountingRenderer first;
            EXPECT_EQ(
                built.hand(first, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = scene })
                    .mKind,
                SceneUpload::Kind::Rebuilt);
            EXPECT_EQ(
                built.hand(first, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = scene })
                    .mKind,
                SceneUpload::Kind::Placed);

            SceneUploader fresh;
            Testing::CountingRenderer second;
            EXPECT_EQ(
                fresh.hand(second, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = scene })
                    .mKind,
                SceneUpload::Kind::Rebuilt);
        }

        /// A renderer carrying somebody else's textures is built from nothing, not appended to.
        ///
        /// **This is `bench`, and it is what a crash looked like.** It runs several places through
        /// one renderer with a scene of its own for each, so the second place meets an array holding
        /// the first place's images. An uploader that read that count as its own would start
        /// describing at 375 in a table 231 long, which is not a wrong picture but an overrun — and
        /// it surfaced as an allocation failure from somewhere with nothing to do with textures.
        TEST(RtxSceneUploaderTest, aSecondSceneOnOneRendererIsBuiltRatherThanAppendedTo)
        {
            Testing::CountingRenderer renderer;

            // The longer scene first, so appending onto its count would run off the end of the
            // shorter one — which is the failure, rather than merely describing the wrong images.
            Rtx::SceneDesc crowded;
            Testing::addModel(crowded, VFS::Path::NormalizedView("textures/one.dds"));
            Testing::addModel(crowded, VFS::Path::NormalizedView("textures/two.dds"));
            Testing::addModel(crowded, VFS::Path::NormalizedView("textures/three.dds"));

            SceneUploader place;
            ASSERT_EQ(
                place.hand(renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = crowded })
                    .mKind,
                SceneUpload::Kind::Rebuilt);
            ASSERT_EQ(renderer.mTextures, 3u);

            Rtx::SceneDesc sparse;
            Testing::addModel(sparse, VFS::Path::NormalizedView("textures/four.dds"));

            SceneUploader next;
            const SceneUpload second = next.hand(
                renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = sparse });
            EXPECT_EQ(second.mKind, SceneUpload::Kind::Rebuilt);
            EXPECT_EQ(second.mDescribed, std::size_t{ 1 }) << "the descriptions began past the end of the table";
            EXPECT_EQ(renderer.mTextures, 1u);

            // And the same uploader carries on with the scene it did build, so the guard costs the
            // ordinary frame nothing.
            EXPECT_EQ(
                next.hand(renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = sparse })
                    .mKind,
                SceneUpload::Kind::Placed);

            // **A description is what it is and not where it is.** Moved, it keeps its identity
            // and the slot built from it is still its own; a description made after it is
            // another, at whatever address, and gets a build of its own.
            Rtx::SceneDesc moved = std::move(sparse);
            EXPECT_EQ(moved.getIdentity(), renderer.describeHeld(Rtx::SceneSlot::world()).mIdentity);
            EXPECT_EQ(
                next.hand(renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = moved })
                    .mKind,
                SceneUpload::Kind::Placed)
                << "a move was taken for another description";

            Rtx::SceneDesc another;
            EXPECT_NE(another.getIdentity(), moved.getIdentity());
            EXPECT_NE(another.getIdentity(), 0u);
        }
        /// A picture inside the interface is handed over the same way a cell is, and neither
        /// disturbs the other.
        ///
        /// **This is what a race-creation slider drag costs.** The doll is walked again on every
        /// frame of the drag — the placements thrown away and refilled — and nothing about it
        /// arrives or goes, so the second hand and every one after it is a placement. Rebuilding it
        /// each time is an acceleration structure and a texture array made from nothing sixty times
        /// a second.
        TEST(RtxSceneUploaderTest, aPictureIsHandedOverTheSameWayACellIsAndNeitherDisturbsTheOther)
        {
            Testing::CountingRenderer renderer;

            Rtx::SceneDesc world;
            Testing::addModel(world, VFS::Path::NormalizedView("textures/ground.dds"));
            Testing::addModel(world, VFS::Path::NormalizedView("textures/wall.dds"));

            Rtx::SceneDesc doll;
            const Testing::Model body = Testing::addModel(doll, VFS::Path::NormalizedView("textures/skin.dds"));

            const Rtx::SceneSlot slot = renderer.addViewScene();

            SceneUploader ofTheWorld;
            SceneUploader ofTheDoll;

            ASSERT_EQ(
                ofTheWorld
                    .hand(renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = world })
                    .mKind,
                SceneUpload::Kind::Rebuilt);
            ASSERT_EQ(ofTheDoll.hand(renderer, Rtx::SceneUploader::Handing{ .mSlot = slot, .mScene = doll }).mKind,
                SceneUpload::Kind::Rebuilt);

            // Each table is its own length, and building the doll did not append onto the world's.
            EXPECT_EQ(renderer.describeHeld(Rtx::SceneSlot::world()).mTextureCount, 2u);
            EXPECT_EQ(renderer.describeHeld(slot).mTextureCount, 1u);
            EXPECT_FALSE(renderer.mAppendedToWrongEnd);

            // The drag: the placements are thrown away and the same body walked back in, which is
            // what `SceneExtractor` does when its identity maps recognise every drawable — it places
            // and adds nothing.
            for (int frame = 0; frame < 3; ++frame)
            {
                doll.clearPlacement();
                doll.addInstance(Rtx::MeshInstance{ .mMesh = body.mMesh, .mMaterial = body.mMaterial });

                EXPECT_EQ(ofTheDoll.hand(renderer, Rtx::SceneUploader::Handing{ .mSlot = slot, .mScene = doll }).mKind,
                    SceneUpload::Kind::Placed)
                    << "redraw " << frame << " rebuilt a subject that did not change";
            }

            // And the world is still the world's: the doll's frames did not make its next one a
            // rebuild.
            world.clearPlacement();
            world.addInstance(Rtx::MeshInstance{ .mMesh = 0, .mMaterial = 0 });
            EXPECT_EQ(
                ofTheWorld
                    .hand(renderer, Rtx::SceneUploader::Handing{ .mSlot = Rtx::SceneSlot::world(), .mScene = world })
                    .mKind,
                SceneUpload::Kind::Placed);
            EXPECT_EQ(renderer.describeHeld(Rtx::SceneSlot::world()).mTextureCount, 2u);
            EXPECT_EQ(renderer.describeHeld(slot).mTextureCount, 1u);
        }

    }
}

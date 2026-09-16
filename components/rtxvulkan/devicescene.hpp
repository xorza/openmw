#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include <components/rtx/instancerecord.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/slot.hpp>
#include <components/rtx/texturedata.hpp>

#include "frameslots.hpp"
#include "sceneacceleration.hpp"
#include "scenebuffers.hpp"
#include "skintables.hpp"
#include "texture.hpp"

namespace Rtx
{
    class Batch;
    class Device;
    class GpuTimer;
    class SceneDesc;
    class SkinPass;
    struct Placing;

    /// Everything one scene is traced against, on the device — the world's, or a picture's in
    /// the interface — the same objects for both, which is what lets `Rtx::SceneUploader` hand a
    /// doll over exactly as a cell. Made whole from one description in one submit, and torn down
    /// whole: a scene that is replaced is destroyed before its successor is built, so the two are
    /// never held at once — a cell's structures and textures are most of what a renderer occupies.
    ///
    /// The passes are shared and stay with the renderer: every texture array declares the same
    /// bindless layout, and identically defined layouts are compatible.
    class DeviceScene
    {
    public:
        /// Builds every structure, table and texture of `scene` into `batch`, which the caller
        /// flushes: one submit for the whole cell, where a round trip apiece would be hundreds for
        /// a town. Every scene is traced by two frames at once, the doll's included — a picture
        /// inside the interface rides the frame it was asked on, and the next frame may place it
        /// again while that one is still tracing — so every table has `sFrameSlots` copies.
        ///
        /// @param skin what poses this scene's bodies, at the build and at every placement.
        DeviceScene(const Device& device, Batch& batch, const SetLayout& textureLayout, const SkinPass& skin,
            const SceneDesc& scene, std::span<const TextureData> textures);

        /// Takes in what the scene says arrived: the textures, and the meshes where the mesh table's
        /// revision moved — the geometry blocks are appended to rather than replaced, so every
        /// address a structure was built from is still its own. The revision and not the count,
        /// because a freed slot taken over holds different geometry at the same size. Into
        /// `batch`, which the caller defers or flushes.
        ///
        /// @param timer where the arrived meshes' build is timed, or null for a picture's scene.
        void extend(Batch& batch, const SceneDesc& scene, std::span<const TextureData> arrived, GpuTimer* timer);

        /// Everything a placement of this scene is, recorded and written where `placing` says. True
        /// where anything was recorded, which is whether its command buffer is worth submitting.
        /// What differs between the world's placement and a picture's is around this and not in it.
        bool place(const SceneDesc& scene, const Placing& placing);

        /// Waits until nothing on the queue reads or writes `slot`'s copy of any table a placement
        /// writes from the host. Asked of each table, which carries the value itself.
        void finishReads(FrameSlot slot) const;

        /// Destroys the images of `textures`, leaving the slots where they are: `TextureArray::drop`.
        void dropTextures(std::span<const Index> textures) { mTextures.drop(textures); }

        /// Which copy of the tables the last placement wrote — what a trace of this scene reads,
        /// and the copy the next placement leaves alone. A placement's parity and not a frame's,
        /// because a frame need not place; per scene, so a doll redrawn on consecutive frames
        /// places exactly as the world does.
        FrameSlot getSlot() const { return mSlot; }
        void placed(FrameSlot into) { mSlot = into; }

        /// Whether a picture of `slot`'s copy is recorded and carried by nothing yet — it rides
        /// `next`, the submit that is next when this is asked. Until it is carried its trace has to
        /// find the copy as it was placed for it: the top level a deferred placement built and the
        /// rows a later placement wrote from the host would otherwise disagree about which instance
        /// is which. So a placement into such a copy carries the picture first. Not a memory hazard,
        /// which the tables' own stamps answer.
        bool pictureRides(FrameSlot slot, std::uint64_t next) const { return mPictureRides[slot.get()] == next; }
        void notePictureRide(FrameSlot slot, std::uint64_t value) { mPictureRides[slot.get()] = value; }

        /// What this scene holds, as `Renderer::describeHeld` answers it.
        SceneHeld describe() const;

        /// Reads into `stats` what a placement can have moved, which is every figure but the three
        /// a build settles — one of which is a loop over every texture, and a placement runs on the
        /// frame path.
        void readPlacedStats(SceneStats& stats) const;

        /// Reads all of `stats`, for a scene that has just been built or extended.
        void readStats(SceneStats& stats) const;

        /// What the copy the last placement wrote counts as, as the scene counted it then: the
        /// trace of that copy reads this, and the scene may have moved on since.
        const InstanceCounts& getCounts() const { return mCounts; }
        const SceneAcceleration& getAcceleration() const { return mAcceleration; }
        const SceneBuffers& getBuffers() const { return mBuffers; }

        /// The set the trace binds: the copy the last placement wrote, brought up to date by it.
        VkDescriptorSet getTextures() const { return mTextures.getSet(mSlot); }

    private:
        const SkinPass& mSkin;

        /// One row per placement slot, made whole when the scene is built and kept across frames,
        /// with the rows the scene says changed rewritten by each placement. Here rather than in
        /// either half, because the acceleration structure and the instance table need the same
        /// rows, and each building its own was fifty thousand matrix inverses on a nine-by-nine
        /// exterior. Before the halves, which are built from it.
        std::vector<InstanceRecord> mRecords;

        /// Which of those `updateInstanceRecords` wrote this placement, cleared and refilled. One
        /// list read by both halves, because two answers to which changed is one of them wrong and
        /// terrain a frame behind.
        std::vector<Index> mChangedRecords;

        SceneAcceleration mAcceleration;
        SceneBuffers mBuffers;

        /// What this scene's skinned bodies and morphed faces are posed from.
        SkinTables mSkinTables;
        TextureArray mTextures;

        /// Which revision of the mesh table the structures were built from, so `extend` can tell
        /// a scene that only gained textures from one that gained geometry too.
        std::uint64_t mBuiltMeshes = 0;

        /// The revision of the whole structure this was built from: what `describe` answers and
        /// an uploader appends against.
        std::uint64_t mBuiltStructure = 0;

        /// Which description that was, as `SceneHeld::mScene` reports it.
        const SceneDesc* mBuiltFrom = nullptr;

        FrameSlot mSlot;

        /// The scene's counts as they stood at the last placement — `PlacementTable::getCounts`,
        /// copied because the placement is the last time this reads the scene before its trace.
        InstanceCounts mCounts;

        /// The submit a picture of each copy rides, as the timeline value it was recorded for.
        std::array<std::uint64_t, sFrameSlots> mPictureRides{};
    };
}

#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "material.hpp"
#include "runs.hpp"
#include "scenedesc.hpp"

namespace Rtx
{
    /// How many chunks one `advance` may hand to the device to flatten — a bound on what an
    /// arrival frame pays, because every composite taken is a texture stood in the array and a
    /// dispatch over a quarter of a million texels of it. The rest wait a frame each, shading from
    /// their stacks.
    inline constexpr std::size_t sCompositesPerFrame = 2;

    /// Every distant chunk whose ground is to be flattened, in the order the walks asked. A chunk
    /// that asks is given a slot and its material's `mDiffuse`, and the slot arrives at the
    /// device empty: `GroundCompositePass` sums the chunk's own stack into it in the placement
    /// after, reading the layers' textures and masks the device already holds. Nothing is wrong
    /// while a chunk waits: it keeps `mDiffuse` unset and the shader sums its layer stack at the
    /// hit, so the bake buys the cost of that hit and not the sight of the ground.
    ///
    /// **No threads and no bytes.** This flattened stacks on threads of its own once, tens of
    /// milliseconds a chunk and a megabyte and a half staged for each, and a frame had to wait
    /// for the thread to keep the frame a composite landed on the schedule's answer. The frame a
    /// chunk is taken on is now a count, `sCompositesPerFrame` a frame in the order asked, which
    /// is the same count on every run.
    class CompositeQueue
    {
    public:
        /// Hands the chunks this walk marked to the schedule, then takes the next few in order,
        /// and says how many. Before anything reads what arrived, because a chunk taken takes a
        /// texture slot and is an arrival like any other.
        std::size_t advance(SceneDesc& scene);

        /// The material whose ground the composite in `slot` is, or `sNoIndex` where nothing here
        /// gave that slot out this frame.
        Index find(Index slot) const;

        /// Lets go of what `advance` gave out, after the arrival that described it.
        void releaseFinished() { mFinished.clear(); }

    private:
        /// Which chunk asked: the material's slot and where its layers sat when it did, so a slot
        /// another chunk took over in the meantime is not handed the first one's ground.
        struct Asked
        {
            Index mMaterial = sNoIndex;
            Run mLayers;

            bool operator==(const Asked& other) const = default;
        };

        /// Puts every chunk the walk wrote that wants flattening and is not already waiting onto
        /// the schedule, off the rows the scene says it wrote and never by scanning the table.
        void gather(const SceneDesc& scene);

        /// Takes the chunks at the front of the schedule, at most `limit` of them, and says how
        /// many. A chunk taken takes a texture slot and goes onto the material that asked; one
        /// whose slot another chunk took over while it waited is dropped.
        std::size_t take(SceneDesc& scene, std::size_t limit);

        /// Oldest first: the order the walks asked in, which is the order the device flattens in.
        std::deque<Asked> mWaiting;

        /// A slot given out, and the chunk it is the ground of.
        struct Given
        {
            Index mSlot = sNoIndex;
            Index mMaterial = sNoIndex;
        };

        /// What `advance` gave out this frame, at most `sCompositesPerFrame` of them. Emptied by
        /// `releaseFinished` and never freed.
        std::vector<Given> mFinished;

        std::string mKey;
    };
}

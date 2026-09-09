#pragma once

#include <cstdint>

namespace Rtx
{
    /// How many instances a scene places, and how many of those each kind of traversal has to stop
    /// for.
    ///
    /// **One record, held by whoever keeps the rows and handed on unchanged to whoever reports
    /// them.** Every one of these is a count the placement already keeps — kept by the row that
    /// changed rather than recounted over the table — so a second set of fields on the way out is a
    /// field added in one place and forgotten in the other.
    struct InstanceCounts
    {
        std::uint32_t mPlaced = 0;

        /// How many of those traversal has to stop and ask where the holes are — the cost of the
        /// cutout, as a number, so a material change that marks half a cell non-opaque shows up
        /// before a frame time does.
        ///
        /// **Not every instance traversal stops for.** A translucent one is stopped for as well, and
        /// is counted here nowhere: what it costs is a different question, since it never ends the
        /// ray.
        std::uint32_t mCutout = 0;

        /// How many of them the eye meets as water.
        ///
        /// **What says whether a trace needs the sea at all.** A frame's water level says where a
        /// surface would be and not whether there is one, and a room with neither is a kernel with
        /// no waves, no caustics and no underwater column in it — `HAS_SEA` is what removes them.
        std::uint32_t mWater = 0;

        /// How many of them are a medium the eye passes through — `Rtx::Material::isMedium`.
        ///
        /// **What says whether the trace has to gather one at all.** `mediumAlong` walks the
        /// structure on a mask of its own, and where no instance carries that mask the walk still
        /// descends the top level and finds nothing: measured at 0.02 ms of a 1.86 ms trace over
        /// Seyda Neen. `VisibilityConstants::mMediumInFrame` is what carries this to the shader.
        std::uint32_t mMedium = 0;
    };
}

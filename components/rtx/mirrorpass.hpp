#pragma once

#include <cassert>
#include <cstdint>

#include "extractionstats.hpp"

namespace Rtx
{
    /// What one walk is: which sweep stamps it, and where its counts go.
    ///
    /// **Borrowed by every resolver**, so a pass is one state rather than four copies free to fall
    /// behind each other. The epoch was already shared for that reason, and the counts are the
    /// other half of the same fact — what this pass met, as against what the last one did.
    ///
    /// **Borrowed `const`, because only the extractor moves a pass on.** A resolver reads the epoch
    /// and writes the counts through it, and neither advancing the sweep nor repointing the counts
    /// is a resolver's to do.
    struct MirrorPass
    {
        std::uint64_t mEpoch = 0;

        /// Null between walks. A walk is not re-entrant — the extractor's anchor is a member set
        /// per walk — so there is one of these at a time, and `getStats` is what says so.
        ExtractionStats* mStats = nullptr;

        ExtractionStats& getStats() const
        {
            assert(mStats != nullptr && "a resolver reached outside a walk");
            return *mStats;
        }
    };
}

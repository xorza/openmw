#include "extractionstats.hpp"

#include <array>

namespace Rtx
{
    namespace
    {
        /// Every counter the sum adds, by member pointer and not by structured binding, because a
        /// binding takes members by position and a field inserted mid-struct shifts every name
        /// after it without a diagnostic. A counter added to the struct and not here is what the
        /// sum test catches.
        constexpr std::array sCounters{
            &ExtractionStats::mMeshesAdded,
            &ExtractionStats::mMaterialsAdded,
            &ExtractionStats::mSheets,
            &ExtractionStats::mMeshesReused,
            &ExtractionStats::mMaterialsReused,
            &ExtractionStats::mInstances,
            &ExtractionStats::mRestood,
            &ExtractionStats::mDeformed,
            &ExtractionStats::mUnskinned,
            &ExtractionStats::mEmitters,
            &ExtractionStats::mSprites,
            &ExtractionStats::mSkippedUnknown,
            &ExtractionStats::mUndescribedSurfaces,
            &ExtractionStats::mWornBeyondKept,
            &ExtractionStats::mSkippedEmpty,
            &ExtractionStats::mLights,
            &ExtractionStats::mDistantStatics,
            &ExtractionStats::mGroundCells,
        };
    }

    ExtractionStats& ExtractionStats::operator+=(const ExtractionStats& other)
    {
        for (const auto counter : sCounters)
            this->*counter += other.*counter;

        mFoldMs += other.mFoldMs;

        return *this;
    }
}

#include "extractionstats.hpp"

#include <cstddef>

#include <osg/Image>

namespace Rtx
{
    namespace
    {
        /// Every counter the sum adds, by name.
        ///
        /// **By member pointer and not by structured binding**, because a binding takes members by
        /// position and a field inserted mid-struct shifts every name after it without a diagnostic.
        /// A counter added to the struct and not here is what the sum test catches.
        constexpr std::array sCounters{
            &ExtractionStats::mMeshesAdded,
            &ExtractionStats::mMaterialsAdded,
            &ExtractionStats::mSheets,
            &ExtractionStats::mMeshesReused,
            &ExtractionStats::mMaterialsReused,
            &ExtractionStats::mInstances,
            &ExtractionStats::mDeformed,
            &ExtractionStats::mUnskinned,
            &ExtractionStats::mEmitters,
            &ExtractionStats::mSprites,
            &ExtractionStats::mSkippedUnknown,
            &ExtractionStats::mUndescribedSurfaces,
            &ExtractionStats::mSpritelessEmitters,
            &ExtractionStats::mSkippedEmpty,
            &ExtractionStats::mLights,
            &ExtractionStats::mWornOtherwise,
            &ExtractionStats::mDistantStatics,
            &ExtractionStats::mGroundCells,
        };
    }

    void FormatCensus::count(const osg::Image& image)
    {
        const ImageFormat format = readFormat(image);

        FormatCount& met = mMet[static_cast<std::size_t>(format)];
        ++met.mMet;
        if (image.getNumMipmapLevels() > 1)
            ++met.mMipped;

        if (format == ImageFormat::Unnamed)
            mUnnamed = static_cast<std::uint32_t>(image.getPixelFormat());
    }

    FormatCensus& FormatCensus::operator+=(const FormatCensus& other)
    {
        for (std::size_t at = 0; at < mMet.size(); ++at)
        {
            mMet[at].mMet += other.mMet[at].mMet;
            mMet[at].mMipped += other.mMet[at].mMipped;
        }

        if (other.mUnnamed != 0)
            mUnnamed = other.mUnnamed;

        return *this;
    }

    ExtractionStats& ExtractionStats::operator+=(const ExtractionStats& other)
    {
        for (const auto counter : sCounters)
            this->*counter += other.*counter;

        mFoldMs += other.mFoldMs;
        mFormats += other.mFormats;

        return *this;
    }
}

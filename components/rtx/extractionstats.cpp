#include "extractionstats.hpp"

#include <cstddef>

#include <osg/Image>

namespace Rtx
{
    namespace
    {
        /// Every plain counter of `stats`, as one list.
        ///
        /// **The one place they are named, and a binding takes every member or none.** A field
        /// added to `ExtractionStats` and not added here does not compile, which is what keeps the
        /// sum below from dropping one and reporting a number that is short.
        auto countersOf(auto& stats)
        {
            auto& [meshesAdded, materialsAdded, sheets, composites, meshesReused, materialsReused, instances, deformed,
                unskinned, emitters, sprites, skippedUnknown, undescribedMaterials, formats, unnamedFormat,
                skippedEmpty, lights, unbakeable, wornOtherwise]
                = stats;

            // The two the sum owes something other than addition, and so the two left out of it.
            (void)formats;
            (void)unnamedFormat;

            return std::array{ &meshesAdded, &materialsAdded, &sheets, &composites, &meshesReused, &materialsReused,
                &instances, &deformed, &unskinned, &emitters, &sprites, &skippedUnknown, &undescribedMaterials,
                &skippedEmpty, &lights, &unbakeable, &wornOtherwise };
        }
    }

    void countFormat(const osg::Image& image, ExtractionStats& stats)
    {
        const ImageFormat format = readFormat(image);

        FormatCount& count = stats.mTextureFormats[static_cast<std::size_t>(format)];
        ++count.mMet;
        if (image.getNumMipmapLevels() > 1)
            ++count.mMipped;

        if (format == ImageFormat::Unnamed)
            stats.mUnnamedFormat = static_cast<std::uint32_t>(image.getPixelFormat());
    }

    ExtractionStats& ExtractionStats::operator+=(const ExtractionStats& other)
    {
        const auto sum = countersOf(*this);
        const auto add = countersOf(other);
        for (std::size_t at = 0; at < sum.size(); ++at)
            *sum[at] += *add[at];

        for (std::size_t at = 0; at < mTextureFormats.size(); ++at)
        {
            mTextureFormats[at].mMet += other.mTextureFormats[at].mMet;
            mTextureFormats[at].mMipped += other.mTextureFormats[at].mMipped;
        }

        if (other.mUnnamedFormat != 0)
            mUnnamedFormat = other.mUnnamedFormat;

        return *this;
    }
}

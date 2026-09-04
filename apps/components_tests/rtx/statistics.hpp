#pragma once

#include <span>

namespace Rtx::Testing
{
    /// The mean of `values`, summed in double although both ends are float.
    ///
    /// A float accumulator loses the low bits of every term once the running total is a few orders
    /// above the term, and these are frames and fields of a few thousand samples — which is the
    /// error a test that names its expectation to three decimal places has no budget for.
    inline float meanOf(std::span<const float> values)
    {
        double total = 0.0;
        for (const float value : values)
            total += double{ value };

        return static_cast<float>(total / static_cast<double>(values.size()));
    }
}

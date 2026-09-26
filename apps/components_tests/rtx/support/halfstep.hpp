#pragma once

#include <algorithm>
#include <cmath>

namespace Rtx::Testing
{
    /// The distance between the two half floats either side of `value`: what one reading kept
    /// in halves can be wrong by, at most half of it each way. The subnormal spacing under
    /// 2^-14, where the exponent stops falling.
    inline float halfStepAt(float value)
    {
        return std::ldexp(1.0f, std::max(std::ilogb(value), -14) - 10);
    }
}

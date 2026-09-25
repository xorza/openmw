#pragma once

#include <cstdint>

namespace Rtx
{
    /// The `index`th term of the radical inverse in `base`, which is Halton's whole definition:
    /// write the index in that base and reflect its digits about the point. In base two it is Van der
    /// Corput's sequence, exact in float for the first sixteen million terms, and the second
    /// coordinate of a Hammersley set.
    inline float radicalInverse(std::uint32_t index, std::uint32_t base)
    {
        float result = 0.0f;
        float place = 1.0f / static_cast<float>(base);

        while (index > 0)
        {
            result += static_cast<float>(index % base) * place;
            index /= base;
            place /= static_cast<float>(base);
        }

        return result;
    }
}

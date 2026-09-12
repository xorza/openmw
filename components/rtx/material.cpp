#include "material.hpp"

namespace Rtx
{
    namespace
    {
        /// What a blended material is tested against when it named no threshold of its own. Half,
        /// because the alpha it is standing in for is very nearly binary already: Morrowind's masks
        /// are painted, not anti-aliased, and the fringe a filter puts on them is a texel wide.
        constexpr float sBlendCutoff = 0.5f;
    }

    float Material::getAlphaCutoff() const
    {
        switch (mAlphaMode)
        {
            case AlphaMode::Opaque:
                return 0.0f;
            case AlphaMode::Cutout:
                return mAlphaRef;
            case AlphaMode::Blend:
                return mAlphaRef > 0.0f ? mAlphaRef : sBlendCutoff;
        }

        return 0.0f;
    }
}

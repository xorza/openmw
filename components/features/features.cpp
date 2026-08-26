#include "features.hpp"

namespace Features
{
    bool hasRayTracing()
    {
#ifdef OPENMW_RTX
        return true;
#else
        return false;
#endif
    }
}

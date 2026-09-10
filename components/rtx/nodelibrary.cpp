#include "nodelibrary.hpp"

#include <cstring>

namespace Rtx
{
    namespace
    {
        /// What a library name says, worked out the once for each address it is spelled at.
        Library read(const char* name)
        {
            if (std::strcmp(name, "NifOsg") == 0)
                return Library::NifOsg;
            if (std::strcmp(name, "osgParticle") == 0)
                return Library::OsgParticle;
            if (std::strcmp(name, "SceneUtil") == 0)
                return Library::SceneUtil;

            return Library::Other;
        }
    }

    Library NodeLibrary::learn(const char* name) const
    {
        const Library answer = read(name);

        if (mHeld < mNames.size())
        {
            mNames[mHeld] = name;
            mAnswers[mHeld] = answer;
            ++mHeld;
        }

        return answer;
    }
}

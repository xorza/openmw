#include "raymask.hpp"

#include <components/rtx/shaders/scene.h>
#include <components/sceneutil/vismask.hpp>

namespace MWRender
{
    std::uint32_t rayMaskOf(const osg::Node::NodeMask cullMask)
    {
        using namespace SceneUtil;

        std::uint32_t mask = 0;
        if ((cullMask & (Mask_Object | Mask_Static | Mask_Terrain | Mask_Groundcover)) != 0)
            mask |= Rtx::Shaders::MASK_STATIC;
        if ((cullMask & (Mask_Actor | Mask_Player)) != 0)
            mask |= Rtx::Shaders::MASK_ACTOR;
        if ((cullMask & Mask_Effect) != 0)
            mask |= Rtx::Shaders::MASK_EFFECT;
        if ((cullMask & Mask_FirstPerson) != 0)
            mask |= Rtx::Shaders::MASK_FIRST_PERSON;
        if ((cullMask & (Mask_Water | Mask_SimpleWater)) != 0)
            mask |= Rtx::Shaders::MASK_WATER;
        if ((cullMask & (Mask_ParticleSystem | Mask_WeatherParticles)) != 0)
            mask |= Rtx::Shaders::MASK_PARTICLE;

        // No `MASK_MEDIUM`: a medium is gathered by a ray that casts with that bit alone, whatever
        // the camera, and in the eye's own mask it would meet the shells of a class left out.
        return mask;
    }
}

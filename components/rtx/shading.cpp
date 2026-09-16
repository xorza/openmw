#include "shading.hpp"

#include <string>

#include <osg/StateSet>
#include <osg/Uniform>

#include "surface.hpp"

namespace Rtx
{
    bool describeSurface(std::span<const Shading> shading, SurfaceDescription& material)
    {
        SurfaceLocks locks;
        bool said = false;
        for (const Shading& link : shading)
            said = describeStateSet(*link.mStateSet, material, locks) || said;

        return said;
    }

    float fadeThrough(const osg::StateSet& stateSet, float inherited)
    {
        // Asked of the list before the name, because nearly every state set in the world has no
        // uniform at all. `osg::StateSet::getUniform` searches a `std::map` keyed on
        // `std::string`, and this is called at every node and every drawable a walk enters — a
        // tree walk and a `memcmp` apiece, tens of thousands of times a frame. What writes the two
        // uniforms below is `MWRender::TransparencyUpdater`, on the handful of actors the game is
        // fading, so the empty answer is the answer almost every time and it costs one load.
        if (stateSet.getUniformList().empty())
            return inherited;

        // Named once for the process, and not a `std::string` built for every state set of every
        // drawable's chain, every frame.
        static const std::string sActorFade("actorFade");
        static const std::string sAlpha("alpha");

        const osg::Uniform* fade = stateSet.getUniform(sActorFade);
        if (fade == nullptr)
            return inherited;

        float actorFade = 1.0f;
        float alpha = 1.0f;
        fade->get(actorFade);
        if (const osg::Uniform* hidden = stateSet.getUniform(sAlpha))
            hidden->get(alpha);

        return actorFade * alpha;
    }
}

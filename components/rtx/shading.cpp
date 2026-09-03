#include "shading.hpp"

#include <string>

#include <osg/BlendFunc>
#include <osg/StateSet>
#include <osg/Uniform>

#include <components/surface/material.hpp>

namespace Rtx
{
    const Surface::Material* findDescription(std::span<const Shading> shading)
    {
        for (auto it = shading.rbegin(); it != shading.rend(); ++it)
            if (const Surface::Material* found = Surface::getMaterial(*it->mStateSet))
                return found;

        return nullptr;
    }

    bool addsLight(std::span<const Shading> shading)
    {
        for (auto it = shading.rbegin(); it != shading.rend(); ++it)
        {
            const auto* blend
                = dynamic_cast<const osg::BlendFunc*>(it->mStateSet->getAttribute(osg::StateAttribute::BLENDFUNC));
            if (blend == nullptr)
                continue;

            return blend->getSource() == osg::BlendFunc::SRC_ALPHA && blend->getDestination() == osg::BlendFunc::ONE;
        }

        return false;
    }

    float fadeThrough(const osg::StateSet& stateSet, float inherited)
    {
        // Named once for the process. A `std::string` built for every state set of every
        // drawable's chain, every frame, was a measurable share of the walk.
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

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
            // **`static_cast`, because the attribute is keyed by type.** A state set answers
            // `BLENDFUNC` with a `BlendFunc` or with nothing, and this runs once per state set of
            // every drawable's chain — which is the frame path.
            const auto* blend
                = static_cast<const osg::BlendFunc*>(it->mStateSet->getAttribute(osg::StateAttribute::BLENDFUNC));
            if (blend == nullptr)
                continue;

            return blend->getSource() == osg::BlendFunc::SRC_ALPHA && blend->getDestination() == osg::BlendFunc::ONE;
        }

        return false;
    }

    float fadeThrough(const osg::StateSet& stateSet, float inherited)
    {
        // **Asked of the list before the name, because nearly every state set in the world has no
        // uniform at all.** `osg::StateSet::getUniform` searches a `std::map` keyed on
        // `std::string`, and this is called at every node and every drawable a walk enters — 0.113
        // ms a frame at Seyda Neen, with `stl_tree.h` and `memcmp` under it. What writes the two
        // uniforms below is `MWRender::TransparencyUpdater`, on the handful of actors the game is
        // fading, so the empty answer is the answer almost every time and it costs one load.
        if (stateSet.getUniformList().empty())
            return inherited;

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

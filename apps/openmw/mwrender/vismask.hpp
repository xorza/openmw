#ifndef OPENMW_MWRENDER_VISMASK_H
#define OPENMW_MWRENDER_VISMASK_H

#include <components/sceneutil/vismask.hpp>

namespace MWRender
{
    /// The one table of node masks, lifted so the harness and the scene extractor read it too, under
    /// the names upstream's files spell.
    using enum SceneUtil::VisMask;
    using SceneUtil::sToggleWorldMask;
}

#endif

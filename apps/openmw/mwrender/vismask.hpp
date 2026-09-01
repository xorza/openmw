#ifndef OPENMW_MWRENDER_VISMASK_H
#define OPENMW_MWRENDER_VISMASK_H

#include <components/sceneutil/vismask.hpp>

namespace MWRender
{
    /// The one table of node masks, under the name this application spells it by.
    ///
    /// **An alias and not a second copy of any bit.** Every host that stands this graph up marks the
    /// same categories, so the table itself lives in `components/sceneutil/vismask.hpp` where the
    /// harness and the scene extractor can read it too. Naming it `SceneUtil::` at every use instead
    /// would respell every mask in this application, across files that are otherwise upstream's, and
    /// each of those lines is a conflict at the next merge.
    ///
    /// **Which spelling belongs where.** A file this fork wrote names `SceneUtil::` and says so; a
    /// file that came from upstream reads as upstream and takes these. That is the whole rule, and it
    /// is why `mwrender/gl` holds both.
    using enum SceneUtil::VisMask;
    using SceneUtil::sToggleWorldMask;
}

#endif

#pragma once

#include <osg/Callback>

namespace Rtx
{
    /// The first callback of type `T` in a chain, or null where there is none.
    ///
    /// **A chain and not one callback, because OpenSceneGraph hangs them in a list.** A node keeps
    /// one callback and every other one is nested behind it, so anything that asks a node for a
    /// callback of a particular kind has to walk to the end before it can answer no —
    /// `SceneUtil::createLightSource` adds a light's animation behind the callback that collects it,
    /// and `NifOsg` adds its own behind whatever the model already carried.
    template <class T>
    T* findCallback(osg::Callback* chain)
    {
        for (osg::Callback* callback = chain; callback != nullptr; callback = callback->getNestedCallback())
            if (auto* found = dynamic_cast<T*>(callback))
                return found;

        return nullptr;
    }

    template <class T>
    const T* findCallback(const osg::Callback* chain)
    {
        for (const osg::Callback* callback = chain; callback != nullptr; callback = callback->getNestedCallback())
            if (const auto* found = dynamic_cast<const T*>(callback))
                return found;

        return nullptr;
    }
}

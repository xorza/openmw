#pragma once

#include "runs.hpp"

namespace osg
{
    class Drawable;
    class StateSet;
}

namespace Rtx
{
    struct MaterialReading;
    struct MeshReading;

    /// What a residency may do inside the walk that asks it: adopt rows through the mirror's own
    /// resolvers, under the identity the walk would find a clone's mesh under, so that a mesh both
    /// stand is one mesh. An adoption is a hold and a release gives it back, so the sweep keeps a
    /// held entry whatever its stamp.
    class SceneAdopter
    {
    public:
        virtual ~SceneAdopter() = default;

        SceneAdopter(const SceneAdopter&) = delete;
        SceneAdopter& operator=(const SceneAdopter&) = delete;

        /// The material of a reading somebody else made, adopted under the state set it names, with
        /// one hold taken on it. `sNoIndex` and no hold where the reading names no state set.
        virtual Index adoptMaterial(const MaterialReading& reading) = 0;

        /// The same for a mesh, held under the identity the walk will find a clone's mesh under.
        virtual Index adoptMesh(const osg::Drawable& drawable, const MeshReading& reading) = 0;

        /// Gives one hold back on what `adoptMesh` held under `drawable`.
        virtual void releaseMesh(const osg::Drawable& drawable) = 0;

        /// The same for `adoptMaterial`, by the state set the reading named. Nothing for null.
        virtual void releaseMaterial(const osg::StateSet* key) = 0;

    protected:
        SceneAdopter() = default;
    };
}

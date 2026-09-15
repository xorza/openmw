#pragma once

#include <components/esm/refid.hpp>
#include <components/terrain/world.hpp>

namespace osg
{
    class Group;
}

namespace Terrain
{
    class Storage;
    class View;
}

namespace MWRender
{
    /// The ground as the ray tracer stands it: a `Terrain::World` that holds the storage, the
    /// worldspace and the active grid, and builds no chunks — `Rtx::CellRing` reads the land
    /// records itself, and a chunk the game built beside it would be one nothing traces.
    ///
    /// **A world that says it has no chunks**, rather than one the game finds out about: the
    /// preloader asks every world for a view to fill, and the borders a console command toggles
    /// are chunk geometry. Both are answered here, so upstream's callers run unchanged.
    class TracedGround final : public Terrain::World
    {
    public:
        TracedGround(osg::Group& sceneRoot, Terrain::Storage& storage, unsigned int nodeMask, ESM::RefId worldspace);

        /// A view that holds nothing and resets to nothing, because there is nothing to preload
        /// into it. The caller owns it, as `Terrain::World::createView` promises.
        Terrain::View* createView() override;

        /// No chunks, so no borders: the command that toggles them is told they stayed off.
        void setBordersVisible(bool visible) override {}
        bool getBordersVisible() override { return false; }
    };
}

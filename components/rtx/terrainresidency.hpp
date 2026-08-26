#pragma once

#include <osg/Vec3f>
#include <osg/ref_ptr>

#include "sceneextractor.hpp"

namespace Terrain
{
    class View;
    class World;
}

namespace Rtx
{

    /// The terrain's chunks, for a renderer that walks the world rather than culling it.
    ///
    /// **`Terrain::QuadTreeWorld` keeps its chunks out of the scene graph.** It resolves them inside
    /// a cull, against a `ViewData` keyed on the camera doing the culling, and parents them to
    /// nothing — so with `distant terrain` on, a mirror walking the graph finds no ground, no paged
    /// objects and no grass. `Terrain::World::collect` is the way to ask instead of walk, and this is
    /// what holds the view it needs.
    ///
    /// **A view of its own, and one view.** Two would be two sets of chunks at two levels of detail
    /// for one frame, which is what the reflection and the primary ray must not disagree about.
    class TerrainResidency final : public Residency
    {
    public:
        TerrainResidency();
        ~TerrainResidency() override;

        TerrainResidency(const TerrainResidency&) = delete;
        TerrainResidency& operator=(const TerrainResidency&) = delete;

        /// Which world to ask. Changing worldspaces makes a new one, and a view belongs to the world
        /// that handed it out, so the view goes with it.
        void follow(Terrain::World* terrain);

        /// Where the detail is chosen from — the eye, which is what a cull would have used.
        void setViewPoint(const osg::Vec3f& viewPoint) { mViewPoint = viewPoint; }

        void collect(osg::NodeVisitor& visitor) override;

    private:
        Terrain::World* mTerrain = nullptr;

        /// Null for a world that parents its chunks, which is what `createView` answers there. That
        /// world needs nothing from this, and `collect` says so by doing nothing.
        osg::ref_ptr<Terrain::View> mView;

        osg::Vec3f mViewPoint;
    };

}

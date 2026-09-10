#include "distantlights.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include <components/misc/constants.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/terrain/objectstorage.hpp>

#include "distantland.hpp"
#include "lightbuilder.hpp"

namespace Rtx
{
    DistantLights::DistantLights() = default;

    DistantLights::~DistantLights() = default;

    void DistantLights::follow(const Terrain::ObjectStorage* storage, ESM::RefId worldspace)
    {
        if (mStorage == storage && mWorldspace == worldspace)
            return;

        mStorage = storage;
        mWorldspace = worldspace;

        // What was read belongs to the world that was being read.
        restart();
    }

    void DistantLights::restart()
    {
        mCells.clear();
    }

    osg::ref_ptr<osg::Group> DistantLights::build(const osg::Vec2i& cell) const
    {
        std::vector<Terrain::PagedCellRef> refs;
        mStorage->collect(Terrain::RefKind::Lit, 1.0f, cell, mWorldspace, refs);

        osg::ref_ptr<osg::Group> group;

        for (const Terrain::PagedCellRef& ref : refs)
        {
            const std::optional<SceneUtil::LightCommon> light = mStorage->getLight(ref.mRefId);

            // **A reference naming no record is the content's to answer for**, and the game draws
            // nothing for one either. Nothing is invented here to stand in its place.
            if (!light.has_value())
                continue;

            // **At the reference's own origin, and not at the model's `AttachLight` node.** Finding
            // that means loading the mesh, and the mesh is what `pagedType` refuses to stand out
            // here; the offset between the two is the height of a lamp, against a cell of distance.
            // So what `standLight` is handed carries no model, and the light lands on this.
            const osg::ref_ptr<osg::MatrixTransform> place
                = new osg::MatrixTransform(osg::Matrix::translate(ref.mPosition));

            // **The one route both kinds of lamp take**, so what the mirror finds out here is what
            // it would have found had the player walked into the cell — the off-default refusal, the
            // colours, the attenuation and the flicker flags all included. Outdoors is not a guess:
            // the reach is exterior cells and nothing else.
            if (!standLight(*place, *light, /*exterior=*/true))
                continue;

            if (group == nullptr)
                group = new osg::Group;

            group->addChild(place);
        }

        return group;
    }

    void DistantLights::collect(Collector& into)
    {
        if (mStorage == nullptr || !mOutdoors)
            return;

        const int reach = static_cast<int>(std::ceil(mReach / Constants::CellSizeInUnits));
        const osg::Vec2i eye = cellOf(mViewPoint);

        for (int x = eye.x() - reach; x <= eye.x() + reach; ++x)
            for (int y = eye.y() - reach; y <= eye.y() + reach; ++y)
            {
                // **What the game stands for itself is not this class's to stand again.** Inside the
                // active grid the real object is on the graph with its light on it, and the mirror
                // has already met it — see the class comment.
                if (x >= mActiveGrid.x() && y >= mActiveGrid.y() && x < mActiveGrid.z() && y < mActiveGrid.w())
                    continue;

                const osg::Vec2i key(x, y);
                auto found = std::lower_bound(mCells.begin(), mCells.end(), key,
                    [](const ReadCell& held, const osg::Vec2i& wanted) { return held.mCell < wanted; });

                // **Read here rather than on a rota, and never read twice.** What a `LIGH` says is
                // content: it does not change with the hour, the weather or the eye, so a cell costs
                // one reading for the life of the scene and the frames after it cost a pointer. The
                // whole reach is eighty-one cells and reading all of them measured under the
                // run-to-run noise of a still. **A budget per frame would be worse than the spike
                // it avoided**: what a picture holds would then depend on how many frames had been
                // drawn before it, and `verify` compares stills.
                if (found == mCells.end() || found->mCell != key)
                    found = mCells.insert(found, ReadCell{ .mCell = key, .mLights = build(key) });

                if (found->mLights != nullptr)
                    into.take(*found->mLights);
            }
    }
}

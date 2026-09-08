#include "checks.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Vec3f>

#include <components/misc/constants.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/scenetables.hpp>
#include <components/rtxbench/benchrecord.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/world.hpp"

#include "../../mwworld/ptr.hpp"
#include "../../mwworld/refdata.hpp"

#include "worldmirror.hpp"

namespace MWRender
{
    namespace
    {

        /// How wide the square of cells the simulation holds is, in units.
        ///
        /// **What distant ground has to reach past to be distant.** `Constants::CellGridRadius` is
        /// the ring the game loads around the player, so a scene no wider than this is one the
        /// residency contributed nothing to.
        constexpr float sActiveGridWidth
            = static_cast<float>(Constants::CellSizeInUnits) * (2 * Constants::CellGridRadius + 1);
    }

    bool checkHolds(const TracedRun& run, const Rtx::Check check, const Rtx::Crossings& crossings, std::string& found)
    {
        const Rtx::SceneTables scene = run.mScene.getTables();
        const Rtx::ExtractionStats& stats = run.mWalked;

        switch (check)
        {
            case Rtx::Check::WalkTwice:
            {
                const Rtx::ExtractionStats& again = run.mWalkedAgain;
                found = std::format("{} meshes and {} materials added by the second walk, {} drawables resolved",
                    again.mMeshesAdded, again.mMaterialsAdded, again.mMeshesReused);
                return again.mMeshesAdded == 0 && again.mMaterialsAdded == 0 && again.mMeshesReused > 0;
            }

            case Rtx::Check::SurfacesDescribed:
                // **The emitters are reported and not asserted**, for the reason
                // `ExtractionStats::mSpritelessEmitters` gives: every world carries one of the
                // rasterizer's that the traced path answers for itself.
                found = std::format("{} surfaces and {} ground passes undescribed, {} emitters spriteless",
                    stats.mUndescribedSurfaces, stats.mUndescribedGround, stats.mSpritelessEmitters);
                return stats.mUndescribedSurfaces == 0 && stats.mUndescribedGround == 0;

            case Rtx::Check::LightsPlaced:
            {
                const bool indoors = !MWBase::Environment::get().getWorld()->isCellExterior();
                found = std::format("{} lights casting {}", scene.mLights.size(),
                    indoors ? "in a room" : "under a sky, where none is a fair answer");
                return !indoors || !scene.mLights.empty();
            }

            case Rtx::Check::GroundReaches:
            {
                // **Asked of an exterior and answered yes by every room**, which has no distant
                // ground to reach for.
                const bool outdoors = MWBase::Environment::get().getWorld()->isCellExterior();

                // **What stands inside the reach, and not the whole scene's extent.** The sea is
                // one sheet a hundred and fifty cells across, so `getBounds` clears any threshold
                // at every coastline and the question goes unasked. `getContentBoundsWithin` leaves
                // a backdrop out and clips what it meets, which is exactly the ground this is about.
                const osg::Vec3f eye
                    = MWBase::Environment::get().getWorld()->getPlayerPtr().getRefData().getPosition().asVec3();
                const float reach = landReach();
                const float sky = std::numeric_limits<float>::max();
                const osg::BoundingBoxf region(
                    eye.x() - reach, eye.y() - reach, -sky, eye.x() + reach, eye.y() + reach, sky);

                const osg::BoundingBoxf bounds = scene.getContentBoundsWithin(region);
                const float widest
                    = bounds.valid() ? std::max(bounds.xMax() - bounds.xMin(), bounds.yMax() - bounds.yMin()) : 0.0f;

                found = std::format(
                    "the ground spans {:.0f} units against an active grid {:.0f} wide", widest, sActiveGridWidth);
                return !outdoors || widest > sActiveGridWidth;
            }

            case Rtx::Check::LightsNotDoubled:
            {
                std::vector<osg::Vec3f> where;
                where.reserve(scene.mLights.size());
                for (const Rtx::Light& light : scene.mLights)
                    where.push_back(light.mPosition);

                // `osg::Vec3f` orders lexicographically already, which is what a sort for
                // duplicates needs and what its own `operator<` promises.
                std::sort(where.begin(), where.end());

                const auto doubled = std::adjacent_find(where.begin(), where.end());
                found = std::format(
                    "{} lights, {}", where.size(), doubled == where.end() ? "no two at one point" : "two at one point");

                return doubled == where.end();
            }

            case Rtx::Check::TexturesReadable:
                found = std::format(
                    "{} of {} textures could not be read", run.mUnreadableTextures, scene.mTextures.getPaths().size());
                return run.mUnreadableTextures == 0;

            case Rtx::Check::CrossingsAppend:
                found = std::format("{} crossings, {} of them rebuilds", crossings.mCount, crossings.mRebuilds);
                return crossings.mCount > 0 && crossings.mRebuilds < crossings.mCount;
        }

        return false;
    }
}

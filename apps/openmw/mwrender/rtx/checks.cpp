#include "checks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <utility>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Vec3f>

#include <components/misc/constants.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtxbench/benchrecord.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/world.hpp"

#include "../../mwworld/ptr.hpp"
#include "../../mwworld/refdata.hpp"

#include "rtxrenderer.hpp"
#include "worldmirror.hpp"

namespace MWRender
{
    namespace
    {
        /// Every check and the word it is asked for by.
        ///
        /// **The one list of the names**, so a check renamed here is renamed in the command line
        /// and in the report at once.
        constexpr std::array<std::pair<Check, std::string_view>, 8> sChecks{
            std::pair{ Check::WalkTwice, std::string_view("walk-twice") },
            std::pair{ Check::SurfacesDescribed, std::string_view("surfaces-described") },
            std::pair{ Check::LightsPlaced, std::string_view("lights-placed") },
            std::pair{ Check::GroundReaches, std::string_view("ground-reaches") },
            std::pair{ Check::LightsNotDoubled, std::string_view("lights-not-doubled") },
            std::pair{ Check::TexturesReadable, std::string_view("textures-readable") },
            std::pair{ Check::CrossingsAppend, std::string_view("crossings-append") },
            std::pair{ Check::PictureSettles, std::string_view("picture-settles") },
        };

        /// The same list as a run of checks, derived rather than restated: a check added above
        /// reaches the runner without anybody remembering to come here.
        constexpr auto sEvery = [] {
            std::array<Check, sChecks.size()> every{};
            for (std::size_t at = 0; at < sChecks.size(); ++at)
                every[at] = sChecks[at].first;

            return every;
        }();

        /// How wide the square of cells the simulation holds is, in units.
        ///
        /// **What distant ground has to reach past to be distant.** `Constants::CellGridRadius` is
        /// the ring the game loads around the player, so a scene no wider than this is one the
        /// residency contributed nothing to.
        constexpr float sActiveGridWidth
            = static_cast<float>(Constants::CellSizeInUnits) * (2 * Constants::CellGridRadius + 1);
    }

    std::string_view checkName(const Check check)
    {
        for (const auto& [named, spelling] : sChecks)
            if (named == check)
                return spelling;

        return {};
    }

    std::optional<Check> checkNamed(const std::string_view name)
    {
        for (const auto& [named, spelling] : sChecks)
            if (spelling == name)
                return named;

        return std::nullopt;
    }

    std::span<const Check> everyCheck()
    {
        return sEvery;
    }

    bool checkHolds(
        RtxRenderer& owner, const Check check, const Rtx::Crossings& crossings, const bool settled, std::string& found)
    {
        const Rtx::SceneDesc& scene = owner.getMirror().getScene();
        const Rtx::ExtractionStats& stats = owner.getWalkStats();

        switch (check)
        {
            case Check::WalkTwice:
            {
                const Rtx::ExtractionStats& again = owner.getSecondWalkStats();
                found = std::format("{} meshes and {} materials added by the second walk, {} drawables resolved",
                    again.mMeshesAdded, again.mMaterialsAdded, again.mMeshesReused);
                return again.mMeshesAdded == 0 && again.mMaterialsAdded == 0 && again.mMeshesReused > 0;
            }

            case Check::SurfacesDescribed:
                found = std::format("{} placements wore a material nothing described", stats.mUndescribedMaterials);
                return stats.mUndescribedMaterials == 0;

            case Check::LightsPlaced:
            {
                const bool indoors = !MWBase::Environment::get().getWorld()->isCellExterior();
                found = std::format("{} lights casting {}", scene.getLights().size(),
                    indoors ? "in a room" : "under a sky, where none is a fair answer");
                return !indoors || !scene.getLights().empty();
            }

            case Check::GroundReaches:
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

            case Check::LightsNotDoubled:
            {
                std::vector<osg::Vec3f> where;
                where.reserve(scene.getLights().size());
                for (const Rtx::Light& light : scene.getLights())
                    where.push_back(light.mPosition);

                // `osg::Vec3f` orders lexicographically already, which is what a sort for
                // duplicates needs and what its own `operator<` promises.
                std::sort(where.begin(), where.end());

                const auto doubled = std::adjacent_find(where.begin(), where.end());
                found = std::format(
                    "{} lights, {}", where.size(), doubled == where.end() ? "no two at one point" : "two at one point");

                return doubled == where.end();
            }

            case Check::TexturesReadable:
                found = std::format(
                    "{} of {} textures could not be read", owner.getUnreadableTextures(), scene.getTextures().size());
                return owner.getUnreadableTextures() == 0;

            case Check::CrossingsAppend:
                found = std::format("{} crossings, {} of them rebuilds", crossings.mCount, crossings.mRebuilds);
                return crossings.mCount > 0 && crossings.mRebuilds < crossings.mCount;

            case Check::PictureSettles:
                // **The last two measured frames**, which are the settled ones: what is compared is
                // the picture as a person would look at it rather than a channel that may not
                // survive a rebuild.
                found = settled ? "the last two frames are the same picture" : "the last two frames differ";
                return settled;
        }

        return false;
    }
}

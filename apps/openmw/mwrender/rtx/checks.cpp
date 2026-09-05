#include "checks.hpp"

#include <array>
#include <format>
#include <utility>

#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtxbench/benchrecord.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/world.hpp"

#include "rtxrenderer.hpp"

namespace MWRender
{
    namespace
    {
        /// Every check and the word it is asked for by.
        ///
        /// **The one list of the names**, so a check renamed here is renamed in the command line
        /// and in the report at once.
        constexpr std::array<std::pair<Check, std::string_view>, 5> sChecks{
            std::pair{ Check::WalkTwice, std::string_view("walk-twice") },
            std::pair{ Check::SurfacesDescribed, std::string_view("surfaces-described") },
            std::pair{ Check::LightsPlaced, std::string_view("lights-placed") },
            std::pair{ Check::CrossingsAppend, std::string_view("crossings-append") },
            std::pair{ Check::PictureSettles, std::string_view("picture-settles") },
        };

        constexpr std::array<Check, 5> sEvery{
            Check::WalkTwice,
            Check::SurfacesDescribed,
            Check::LightsPlaced,
            Check::CrossingsAppend,
            Check::PictureSettles,
        };
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

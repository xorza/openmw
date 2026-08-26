#include "scene.hpp"

#include <cstdint>
#include <ostream>

#include <components/debug/debugging.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>

#include "cellchoice.hpp"
#include "stagedworld.hpp"
#include "world.hpp"

namespace RtxTool
{
    int runScene(
        World& world, const ESM::Cell& cell, const StagingRequest& request, const ActorRequest& actors, bool twice)
    {
        std::ostream& out = Debug::getRawStdout();

        StagedWorld staged(world, cell, request, actors);

        const Rtx::SceneDesc& scene = staged.getScene();
        const CellReport& report = staged.getReport();

        // **The still world and the people in it, summed.** They arrive by two walks — the region's
        // geometry, then whoever was posed into it — and what the renderer is handed is both.
        Rtx::ExtractionStats stats = staged.getStaged();
        stats += staged.getSettled();

        printCellHeading(cell);

        out << "\nplaced\n"
            << "  instances:            " << scene.getPlacedCount() << '\n'
            << "  meshes:               " << scene.getMeshes().size() << '\n'
            << "  materials:            " << scene.getMaterials().size() << '\n'
            << "  textures:             " << scene.getTextures().size() << '\n'
            << "  triangles:            " << scene.getTriangleCount() << '\n'
            << "  vertex+index bytes:   " << scene.getGeometryBytes() / 1024 << " KiB\n";

        for (const auto& [format, count] : stats.mTextureFormats)
            out << "  " << count << " x " << format << '\n';

        // Which materials traversal will have to stop and ask about, and which of those asked for it
        // outright. The second number being the small one is the point: Morrowind keeps its foliage
        // under `NiAlphaProperty` rather than under an alpha test.
        std::uint32_t cutouts = 0;
        std::uint32_t tested = 0;
        std::uint32_t glowing = 0;
        for (const Rtx::Material& material : scene.getMaterials())
        {
            cutouts += material.isCutout() ? 1 : 0;
            tested += material.mAlphaMode == Rtx::AlphaMode::Cutout ? 1 : 0;
            glowing += material.mEmissiveColour.length2() > 0.0f || material.mEmissive != Rtx::sNoIndex ? 1 : 0;
        }
        out << "  cutout materials:     " << cutouts << ", " << tested << " of them alpha-tested outright\n"
            << "  emissive materials:   " << glowing << '\n'
            << "  lights:               " << staged.getScene().getLights().size() << " casting, ambient "
            << report.mAmbient.x() << ", " << report.mAmbient.y() << ", " << report.mAmbient.z() << '\n'
            << "  deforming drawables:  " << stats.mDeformed << '\n'
            << "  flattened ground:     " << stats.mComposites << " chunks past a cell\n"
            << "  emitters:             " << stats.mEmitters << " holding " << stats.mSprites << " live particles\n"
            << "  residents:            " << staged.getActorCount() << " posed, " << staged.getPropCount()
            << " live props\n";

        out << "\nnot placed\n"
            << "  record type unread:   " << report.mSkipped.mUnknownType << '\n'
            << "  record has no model:  " << report.mSkipped.mNoModel << '\n'
            << "  model would not load: " << report.mUnreadable << '\n'
            << "  unreadable drawables: " << stats.mSkippedUnknown << '\n'
            << "  empty geometry:       " << stats.mSkippedEmpty << '\n'
            << "  undescribed surfaces: " << stats.mUndescribedMaterials << '\n';

        if (twice)
        {
            // **Literally the same graph, walked again**, which is what the game does every frame.
            const Rtx::ExtractionStats total = staged.mirrorAgain();

            out << "\nsecond pass over the same graph\n"
                << "  new meshes:           " << total.mMeshesAdded << " (should be 0)\n"
                << "  new materials:        " << total.mMaterialsAdded << " (should be 0)\n"
                << "  drawables resolved:   " << total.mMeshesReused << " to a known mesh\n";
        }

        return 0;
    }
}

#include "scene.hpp"

#include <osg/Vec3f>

#include <cstdint>
#include <ostream>

#include <components/debug/debugging.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>

#include "cellchoice.hpp"
#include "scenedigest.hpp"
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
            << "  vertex+index bytes:   " << scene.getGeometryBytes() / 1024 << " KiB\n"
            << "  handed over:          " << digestScene(scene) << '\n';

        for (std::size_t at = 0; at < stats.mTextureFormats.size(); ++at)
        {
            const Rtx::FormatCount& count = stats.mTextureFormats[at];
            const auto format = static_cast<Rtx::ImageFormat>(at);

            if (count.mMipped > 0)
                out << "  " << count.mMipped << " x " << Rtx::nameOf(format) << ", with mips\n";
            if (count.mMet > count.mMipped)
                out << "  " << count.mMet - count.mMipped << " x " << Rtx::nameOf(format) << ", one level\n";
            if (count.mMet > 0 && format == Rtx::ImageFormat::Unnamed)
                out << "    which was pixel format " << stats.mUnnamedFormat << '\n';
        }

        // Which materials traversal will have to stop and ask about, which of those asked for it
        // outright, and which of them a cutoff cannot answer for at all. The second and third being
        // the small ones is the point: Morrowind keeps its foliage under `NiAlphaProperty` rather
        // than under an alpha test, and almost nothing it ships is translucent in its own right.
        std::uint32_t cutouts = 0;
        std::uint32_t tested = 0;
        std::uint32_t translucent = 0;
        std::uint32_t glowing = 0;

        // **Counted off the scene and not off a walk's own account.** What a walk reports it met is
        // what *that* walk met, and a staged world is walked twice — so the flattened chunks and the
        // folded sheets, which are met once each and never again, are nought in whichever of the two
        // the report happens to be reading. The scene carries both facts per row.
        std::uint32_t flattened = 0;
        for (const Rtx::Material& material : scene.getMaterials())
        {
            cutouts += material.isCutout() ? 1 : 0;
            tested += material.mAlphaMode == Rtx::AlphaMode::Cutout ? 1 : 0;
            translucent += material.isTranslucent() ? 1 : 0;
            glowing += material.mEmissiveColour.length2() > 0.0f || material.mEmissive != Rtx::sNoIndex ? 1 : 0;
            flattened += material.mFlatten ? 1 : 0;
        }

        std::uint32_t sheets = 0;
        for (const Rtx::MeshRange& mesh : scene.getMeshes())
            sheets += mesh.mShape.mSheet ? 1 : 0;

        const osg::Vec3f& ambient = staged.getLighting().mDaylight.mAmbient;
        out << "  cutout materials:     " << cutouts << ", " << tested << " of them alpha-tested outright\n"
            << "  translucent:          " << translucent << ", which a cutoff cannot answer for\n"
            << "  emissive materials:   " << glowing << '\n'
            << "  lights:               " << staged.getScene().getLights().size() << " casting, ambient " << ambient.x()
            << ", " << ambient.y() << ", " << ambient.z() << '\n'
            << "  deforming drawables:  " << stats.mDeformed << '\n'
            << "  unbakeable cutouts:   " << stats.mUnbakeable << " placements of a mask a controller moves\n"
            << "  flattened ground:     " << flattened << " chunks past a cell\n"
            << "  emitters:             " << stats.mEmitters << " holding " << stats.mSprites << " live particles\n"
            << "  residents:            " << staged.getActorCount() << " posed, " << staged.getPropCount()
            << " live props\n";

        out << "\nnot placed\n"
            << "  record type unread:   " << report.mSkipped.mUnknownType << '\n'
            << "  record has no model:  " << report.mSkipped.mNoModel << '\n'
            << "  model would not load: " << report.mUnreadable << '\n'
            << "  unreadable drawables: " << stats.mSkippedUnknown << '\n'
            << "  unskinned rigs:       " << stats.mUnskinned << " met before an update found their skeleton\n"
            << "  empty geometry:       " << stats.mSkippedEmpty << '\n'
            << "  undescribed surfaces: " << stats.mUndescribedMaterials << '\n'
            << "  worn otherwise:       " << stats.mWornOtherwise
            << " placements wearing another material than their mesh arrived with\n"
            << "  sheets:               " << sheets << " of the meshes, doubled for their backs\n";

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

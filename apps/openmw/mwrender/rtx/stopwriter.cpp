#include "stopwriter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <ios>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <osg/Image>
#include <osg/Vec3f>

#include <components/esm/refid.hpp>
#include <components/files/conversion.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/rtx/extractionstats.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/colour.h>
#include <components/rtx/texels.hpp>
#include <components/rtx/texturebuilder.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/framehashes.hpp>
#include <components/rtxbench/runrecord.hpp>
#include <components/settings/values.hpp>
#include <components/surface/material.hpp>
#include <components/vfs/pathutil.hpp>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/windowmanager.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/cell.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/esmstore.hpp"
#include "../../mwworld/manualref.hpp"
#include "../../mwworld/ptr.hpp"
#include "../../mwworld/refdata.hpp"

#include "../characterpreview.hpp"
#include "../localmap.hpp"
#include "../offscreenview.hpp"
#include "../renderer.hpp"
#include "rtxrenderer.hpp"
#include "session.hpp"

namespace MWRender
{
    namespace
    {
        /// Draws every picture asked for since the frame, and waits for it.
        ///
        /// A stop stands after the frame, and a picture is drawn inside the next one: drawn now
        /// instead, which is a drain a stop may pay and a frame may not.
        void drawPicturesNow(const FrameContext& context)
        {
            context.mRenderer.flushRedraws();
            context.mRenderer.getBackend().finishGuiTraces();
        }
    }

    void StopWriter::write(const FrameContext& context, const FrameReport& report, const Rtx::Actions& actions,
        const StopFacts& facts, Rtx::RunRecord& record)
    {
        const Writing into{ context, report, record };

        if (!actions.mCapture.empty())
            writeCapture(into, actions.mCapture);

        if (actions.mTail)
            reportTail(into);

        if (!actions.mDump.empty())
            writeDump(into, actions.mDump);

        if (actions.mDigest)
            reportScene(into);

        if (!actions.mSheet.empty())
            writeSheet(into, actions.mSheet);

        if (!actions.mMapTile.empty())
            writeMapTile(into, actions.mMapTile);

        if (actions.mDoll.has_value())
            writeDoll(into, actions.mDoll->mWho, actions.mDoll->mFile);

        if (!actions.mFind.empty())
            reportFound(into, actions.mFind);

        if (!actions.mChecks.empty())
            runChecks(into, actions.mChecks, facts);
    }

    void StopWriter::writeCapture(const Writing& into, const std::filesystem::path& file)
    {
        Rtx::Renderer& renderer = into.mContext.mRenderer.getBackend();
        const Rtx::FrameExtents extents = renderer.getExtents();

        renderer.readPixels(mPixels);
        try
        {
            Rtx::writePng(file, extents.mOutputWidth, extents.mOutputHeight, mPixels);
            into.mRecord.note(std::format(
                "wrote {} {}x{}", Files::pathToUnicodeString(file), extents.mOutputWidth, extents.mOutputHeight));

            if (extents.mRenderWidth != extents.mOutputWidth || extents.mRenderHeight != extents.mOutputHeight)
                into.mRecord.note(std::format(", traced at {}x{}", extents.mRenderWidth, extents.mRenderHeight));

            into.mRecord.note("\n");
        }
        catch (const std::exception& failed)
        {
            into.mRecord.note(std::format("could not write {}: {}\n", Files::pathToUnicodeString(file), failed.what()));
            into.mRecord.fail();
        }
    }

    void StopWriter::reportTail(const Writing& into)
    {
        if (!Rtx::hasFrameImage(into.mReport.mReconstruction, Rtx::FrameImage::Accumulated))
        {
            into.mRecord.note(
                std::format("no bounce tail: only the wavelet writes one, and {} put this frame back together\n",
                    Rtx::denoiserName(into.mReport.mReconstruction.mDenoiser)));
            into.mRecord.fail();
            return;
        }

        Rtx::Renderer& renderer = into.mContext.mRenderer.getBackend();

        std::vector<float> bounce;
        renderer.readFrameImage(Rtx::FrameImage::Accumulated, bounce);

        // The ladder the fork's own table was taken on. One is about where the signal ends — a
        // surface seeing a full hemisphere of sky — and everything past it is the tail proper.
        static constexpr std::array<float, 5> sThresholds{ 0.5f, 1.0f, 8.0f, 32.0f, 64.0f };
        std::array<std::uint64_t, 5> over{};

        const std::size_t counted = bounce.size() / 4;
        for (std::size_t at = 0; at < counted; ++at)
        {
            // **The renderer's own weights and not a copy of them.** A second set would be a
            // second idea of which of two things is brighter, and this is what decides which of
            // a frame's pixels are outliers.
            const float lit = bounce[at * 4] * Rtx::Shaders::LUMINANCE_WEIGHTS.x()
                + bounce[at * 4 + 1] * Rtx::Shaders::LUMINANCE_WEIGHTS.y()
                + bounce[at * 4 + 2] * Rtx::Shaders::LUMINANCE_WEIGHTS.z();

            for (std::size_t step = 0; step < sThresholds.size(); ++step)
                if (lit > sThresholds[step])
                    ++over[step];
        }

        into.mRecord.note("bounce tail:");
        for (std::size_t step = 0; step < sThresholds.size(); ++step)
            into.mRecord.note(std::format("{}>{} {:.4f}%", step == 0 ? " " : ", ", sThresholds[step],
                counted > 0 ? static_cast<double>(over[step]) / static_cast<double>(counted) * 100.0 : 0.0));

        into.mRecord.note("\n");
    }

    void StopWriter::writeDump(const Writing& into, const std::filesystem::path& file)
    {
        Rtx::Renderer& renderer = into.mContext.mRenderer.getBackend();

        std::vector<float> radiance;
        renderer.readFrameImage(Rtx::FrameImage::Composite, radiance);

        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(radiance.data()),
            static_cast<std::streamsize>(radiance.size() * sizeof(float)));

        if (!out)
        {
            into.mRecord.note("could not write " + Files::pathToUnicodeString(file) + '\n');
            into.mRecord.fail();
        }
    }

    void StopWriter::reportScene(const Writing& into)
    {
        const Rtx::SceneDesc& scene = into.mContext.mScene;
        const Rtx::ExtractionStats& stats = into.mReport.mWalked.mFound;

        into.mRecord.note(
            std::format("\nplaced\n"
                        "  instances:            {}\n"
                        "  distant statics:      {}\n"
                        "  ground cells:         {}\n"
                        "  meshes:               {}\n"
                        "  materials:            {}\n"
                        "  textures:             {}\n"
                        "  triangles:            {}\n"
                        "  vertex+index bytes:   {} KiB\n"
                        "  handed over:          {}\n"
                        "  laid out as:          {}\n",
                scene.placements().getPlacedCount(), stats.mDistantStatics, stats.mGroundCells,
                scene.meshes().getRows().size(), scene.materials().getRows().size(), scene.textures().getPaths().size(),
                scene.meshes().getTriangleCount(), scene.meshes().getGeometryBytes() / 1024,
                Rtx::spellHash(Rtx::digestScene(scene)), Rtx::spellHash(Rtx::digestLayout(Rtx::digestParts(scene)))));

        for (std::size_t at = 0; at < stats.mFormats.mMet.size(); ++at)
        {
            const Rtx::FormatCount& count = stats.mFormats.mMet[at];
            const auto format = static_cast<Rtx::ImageFormat>(at);

            if (count.mMipped > 0)
                into.mRecord.note(std::format("  {} x {}, with mips\n", count.mMipped, Rtx::nameOf(format)));
            if (count.mMet > count.mMipped)
                into.mRecord.note(
                    std::format("  {} x {}, one level\n", count.mMet - count.mMipped, Rtx::nameOf(format)));
            if (count.mMet > 0 && format == Rtx::ImageFormat::Unnamed)
                into.mRecord.note(std::format("    which was pixel format {}\n", stats.mFormats.mUnnamed));
        }

        // Which materials traversal will have to stop and ask about, which of those asked for it
        // outright, and which of them a cutoff cannot answer for at all. The second and third being
        // the small ones is the point: Morrowind keeps its foliage under `NiAlphaProperty` rather
        // than under an alpha test, and almost nothing it ships is translucent in its own right.
        //
        // **Counted off the scene and not off a walk's own account.** What a walk reports it met is
        // what *that* walk met, and a chunk flattened once is nought in every walk after it. The
        // scene carries both facts per row.
        std::uint32_t cutouts = 0;
        std::uint32_t tested = 0;
        std::uint32_t translucent = 0;
        std::uint32_t media = 0;
        std::uint32_t glowing = 0;
        std::uint32_t flattened = 0;
        for (const Rtx::Material& material : scene.materials().getRows())
        {
            cutouts += material.isCutout() ? 1 : 0;
            tested += material.mAlphaMode == Surface::AlphaMode::Cutout ? 1 : 0;
            translucent += material.isTranslucent() ? 1 : 0;
            media += material.isMedium() ? 1 : 0;
            glowing += material.mEmissiveColour.length2() > 0.0f || material.mEmissive != Rtx::sNoIndex ? 1 : 0;
            flattened += material.mFlatten ? 1 : 0;
        }

        std::uint32_t sheets = 0;
        for (const Rtx::MeshRange& mesh : scene.meshes().getRows())
            sheets += mesh.mShape.mSheet ? 1 : 0;

        into.mRecord.note(
            std::format("  cutout materials:     {}, {} of them alpha-tested outright\n"
                        "  translucent:          {}, which a cutoff cannot answer for\n"
                        "  media:                {} of those are nowhere opaque\n"
                        "  emissive materials:   {}\n"
                        "  lights:               {} casting\n"
                        "  deforming drawables:  {}\n"
                        "  flattened ground:     {} cells outside the active grid\n"
                        "  emitters:             {} holding {} live particles\n",
                cutouts, tested, translucent, media, glowing, scene.lights().size(), stats.mDeformed, flattened,
                stats.mEmitters, stats.mSprites));

        into.mRecord.note(
            std::format("\nnot placed\n"
                        "  unreadable drawables: {}\n"
                        "  unskinned rigs:       {} met before an update found their skeleton\n"
                        "  empty geometry:       {}\n"
                        "  undescribed surfaces: {} drawn as a default material\n"
                        "  spriteless emitters:  {} dropped whole\n"
                        "  worn otherwise:       {} placements wearing another material than their mesh\n"
                        "  sheets:               {} of the meshes, doubled for their backs\n",
                stats.mSkippedUnknown, stats.mUnskinned, stats.mSkippedEmpty, stats.mUndescribedSurfaces,
                stats.mSpritelessEmitters, stats.mWornOtherwise, sheets));

        if (into.mReport.mWalked.mAgain.has_value())
        {
            const Rtx::ExtractionStats& again = *into.mReport.mWalked.mAgain;
            into.mRecord.note(
                std::format("\nsecond pass over the same graph\n"
                            "  new meshes:           {} (should be 0)\n"
                            "  new materials:        {} (should be 0)\n"
                            "  drawables resolved:   {} to a known mesh\n",
                    again.mMeshesAdded, again.mMaterialsAdded, again.mMeshesReused));
        }
    }

    void StopWriter::writeSheet(const Writing& into, const std::filesystem::path& sheet)
    {
        Resource::ResourceSystem* resources = into.mContext.mResources;
        if (resources == nullptr)
            return;

        const Rtx::SceneDesc& scene = into.mContext.mScene;

        Rtx::SceneTextures described;
        described.describeAll(scene, *resources->getImageManager());

        const Rtx::ContactSheet drawn
            = Rtx::writeContactSheet(described.getDescriptions(), sheet, Settings::rtx().mDelight);
        if (drawn.mCount == 0)
        {
            into.mRecord.note("the world uses no textures\n");
            into.mRecord.fail();
            return;
        }

        // The sheet carries no lettering, so the order is printed instead: left to right, top to
        // bottom, the way it was drawn.
        const std::span<const VFS::Path::Normalized> paths = scene.textures().getPaths();
        for (std::size_t at = 0; at < paths.size(); ++at)
            into.mRecord.note(std::format("  {}  {}\n", at, paths[at].value()));

        into.mRecord.note(std::format("wrote {}, {} textures at delight {}\n", Files::pathToUnicodeString(sheet),
            drawn.mCount, static_cast<float>(Settings::rtx().mDelight)));
    }

    void StopWriter::writeView(const Writing& into, OffscreenView& view, const std::filesystem::path& file)
    {
        view.keepCopy();
        view.redraw();
        drawPicturesNow(into.mContext);

        const osg::Image* drawn = view.getCopy();
        if (drawn == nullptr)
        {
            into.mRecord.note("the picture was not drawn\n");
            into.mRecord.fail();
            return;
        }

        writeImage(into, *drawn, file);
    }

    void StopWriter::writeImage(const Writing& into, const osg::Image& drawn, const std::filesystem::path& file)
    {
        const int width = drawn.s();
        const int height = drawn.t();
        const auto stride = static_cast<std::size_t>(width) * 4;
        mPixels.resize(stride * static_cast<std::size_t>(height));

        for (int row = 0; row < height; ++row)
            std::memcpy(
                mPixels.data() + stride * static_cast<std::size_t>(row), drawn.data(0, height - 1 - row), stride);

        Rtx::writePng(file, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), mPixels);
        into.mRecord.note(std::format("wrote {} {}x{}\n", Files::pathToUnicodeString(file), width, height));
    }

    void StopWriter::writeMapTile(const Writing& into, const std::filesystem::path& file)
    {
        // **The game's own tile, and not a picture framed here to look like one.** The local map
        // drew the cell the player stands in when they entered it, at the resolution and over the
        // depth range the settings gave it; what a stop writes is that picture.
        LocalMap* map = MWBase::Environment::get().getWindowManager()->getLocalMap();
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        const MWWorld::Cell& cell = *player.getCell()->getCell();

        const osg::Image* drawn = map != nullptr ? map->getMapImage(cell.getGridX(), cell.getGridY()) : nullptr;
        if (drawn == nullptr && map != nullptr)
        {
            // The first ask starts the copy; asked again after the drain, it is there.
            drawPicturesNow(into.mContext);
            drawn = map->getMapImage(cell.getGridX(), cell.getGridY());
        }

        if (drawn == nullptr)
        {
            into.mRecord.note("no map tile is drawn for the cell the stop stands in\n");
            into.mRecord.fail();
            return;
        }

        writeImage(into, *drawn, file);
    }

    void StopWriter::writeDoll(const Writing& into, const std::string& who, const std::filesystem::path& file)
    {
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        const ESM::RefId id = ESM::RefId::stringRefId(who);

        // **Stood in the world and not assembled beside it.** `MWRender::NpcAnimation` is what
        // dresses a body out of the parts a race calls for, equips what the record carries and
        // finds the bone a weapon hangs on — and it needs a live reference to do any of it.
        const MWWorld::Ptr player = world.getPlayerPtr();
        MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), id, 1);
        const MWWorld::Ptr subject
            = world.placeObject(ref.getPtr(), player.getCell(), player.getRefData().getPosition());

        if (subject.isEmpty())
        {
            into.mRecord.note(std::format("no NPC record is called \"{}\"\n", who));
            into.mRecord.fail();
            return;
        }

        {
            InventoryPreview preview(into.mContext.mRenderer, into.mContext.mResources, subject);
            preview.rebuild();

            // **Through the view and not through the texture the GUI draws from**, which is the one
            // route that carries the row order: `OffscreenView::getTexture` is Y-up and a PNG is not,
            // so a writer reading the texture had to remember a convention and this one did not.
            writeView(into, preview.getView(), file);
        }

        // The subject was a prop for one picture; the stops after this one stand in a cell without it.
        world.deleteObject(subject);
    }

    void StopWriter::reportFound(const Writing& into, const std::string& needle)
    {
        const Rtx::SceneDesc& scene = into.mContext.mScene;
        const std::span<const VFS::Path::Normalized> paths = scene.textures().getPaths();

        // **Found by texture and reported by placement**, because a mesh carries no name of its own
        // once it is a run of triangles: what a walk keeps is the material it arrived wearing, and a
        // material names the file it samples.
        std::uint32_t met = 0;
        for (const Rtx::MeshInstance& instance : scene.placements().getAll())
        {
            if (!instance.isPlaced())
                continue;

            if (instance.mMaterial == Rtx::sNoIndex)
                continue;

            const Rtx::Material& material = scene.materials().getRows()[instance.mMaterial];
            if (material.mDiffuse == Rtx::sNoIndex)
                continue;

            const std::string_view path = paths[material.mDiffuse].value();
            if (path.find(needle) == std::string_view::npos)
                continue;

            const osg::Vec3f at = instance.mTransform.getTrans();
            into.mRecord.note(std::format("  {:.0f}, {:.0f}, {:.0f}   {}\n", at.x(), at.y(), at.z(), path));
            ++met;
        }

        into.mRecord.note(std::format("{} placements wear a texture matching \"{}\"\n", met, needle));
    }

    void StopWriter::runChecks(const Writing& into, const std::span<const Rtx::Check> checks, const StopFacts& facts)
    {
        for (const Rtx::Check check : checks)
        {
            std::string found;
            const bool held = checkHolds(into.mContext, into.mReport, check, facts, found);

            into.mRecord.checked(held);
            into.mRecord.note(std::format("  {:<20} {:<4} {}\n", checkName(check), held ? "ok" : "FAIL", found));
        }
    }
}

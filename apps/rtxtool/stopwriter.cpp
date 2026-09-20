#include "stopwriter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Image>
#include <osg/Math>
#include <osg/Vec2f>
#include <osg/Vec2i>
#include <osg/Vec3f>

#include <apps/openmw/mwbase/environment.hpp>
#include <apps/openmw/mwbase/world.hpp>
#include <apps/openmw/mwrender/camera.hpp>
#include <apps/openmw/mwrender/characterpreview.hpp>
#include <apps/openmw/mwrender/offscreenview.hpp>
#include <apps/openmw/mwrender/renderer.hpp>
#include <apps/openmw/mwrender/renderingmanager.hpp>
#include <apps/openmw/mwrender/rtx/rtxrenderer.hpp>
#include <apps/openmw/mwrender/rtx/tracedview.hpp>
#include <apps/openmw/mwworld/cell.hpp>
#include <apps/openmw/mwworld/cellstore.hpp>
#include <apps/openmw/mwworld/manualref.hpp>
#include <apps/openmw/mwworld/ptr.hpp>
#include <apps/openmw/mwworld/worldmodel.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/refnum.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/constants.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/rtx/cellgrid.hpp>
#include <components/rtx/extractionstats.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/reconstruction.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/surface.hpp>
#include <components/rtx/texels.hpp>
#include <components/rtx/texturebuilder.hpp>
#include <components/rtx/texturedata.hpp>
#include <components/rtxbench/benchrecord.hpp>
#include <components/rtxbench/framehashes.hpp>
#include <components/rtxbench/runrecord.hpp>
#include <components/vfs/pathutil.hpp>

namespace RtxTool
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

        /// Draws every picture asked for since the frame, and waits for it.
        ///
        /// A stop stands after the frame, and a picture is drawn inside the next one: drawn now
        /// instead, which is a drain a stop may pay and a frame may not.
        void drawPicturesNow(const MWRender::FrameContext& context)
        {
            context.mRenderer.drawViews();
            context.mRenderer.getBackend().finishGuiTraces();
        }
    }

    void StopWriter::write(const MWRender::FrameContext& context, const MWRender::FrameReport& report,
        const Rtx::Actions& actions, const StopFacts& facts, Rtx::RunRecord& record)
    {
        const Writing into{ context, report, record };

        if (!actions.mCapture.empty())
            writeCapture(into, actions.mCapture);

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
                        "  vertex+index bytes:   {} KiB, of which vertex colours {} KiB\n"
                        "  handed over:          {}\n"
                        "  laid out as:          {}\n",
                scene.placements().getCounts().mPlaced, stats.mDistantStatics, stats.mGroundCells,
                scene.meshes().getRows().size(), scene.materials().getRows().size(), scene.textures().getRows().size(),
                scene.meshes().getTriangleCount(), scene.meshes().getGeometryBytes() / 1024,
                scene.meshes().getColours().size() * sizeof(osg::Vec3f) / 1024, Rtx::spellHash(Rtx::digestScene(scene)),
                Rtx::spellHash(Rtx::digestLayout(Rtx::digestParts(scene)))));

        for (std::size_t at = 0; at < stats.mFormats.mMet.size(); ++at)
        {
            const Rtx::FormatCount& count = stats.mFormats.mMet[at];
            const auto format = static_cast<Rtx::TextureFormat>(at);

            if (count.mMipped > 0)
                into.mRecord.note(std::format("  {} x {}, with mips\n", count.mMipped, Rtx::nameOf(format)));
            if (count.mMet > count.mMipped)
                into.mRecord.note(
                    std::format("  {} x {}, one level\n", count.mMet - count.mMipped, Rtx::nameOf(format)));
            if (count.mMet > 0 && format == Rtx::TextureFormat::Unnamed)
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
            tested += material.mAlphaMode == Rtx::AlphaMode::Cutout ? 1 : 0;
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
                        "  sheets:               {} of the meshes, doubled for their backs\n",
                stats.mSkippedUnknown, stats.mUnskinned, stats.mSkippedEmpty, stats.mUndescribedSurfaces,
                stats.mSpritelessEmitters, sheets));

        if (into.mReport.mWalked.mAgain.has_value())
        {
            const Rtx::ExtractionStats& again = *into.mReport.mWalked.mAgain;
            into.mRecord.note(
                std::format("\nsecond pass over the same graph\n"
                            "  new meshes:           {} (should be 0)\n"
                            "  new materials:        {} (should be 0)\n"
                            "  drawables resolved:   {} to a known mesh\n"
                            "  stood again:          {} (should be 0)\n",
                    again.mMeshesAdded, again.mMaterialsAdded, again.mMeshesReused, again.mRestood));
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

        const float delight = into.mContext.mRenderer.getProfile().mDelight;
        const Rtx::ContactSheet drawn = Rtx::writeContactSheet(described.getDescriptions(), sheet, delight);
        if (drawn.mCount == 0)
        {
            into.mRecord.note("the world uses no textures\n");
            into.mRecord.fail();
            return;
        }

        // The sheet carries no lettering, so the order is printed instead: left to right, top to
        // bottom, the way it was drawn.
        const std::span<const Rtx::TextureRow> rows = scene.textures().getRows();
        for (std::size_t at = 0; at < rows.size(); ++at)
            into.mRecord.note(std::format("  {}  {}\n", at, rows[at].mPath.value()));

        into.mRecord.note(std::format(
            "wrote {}, {} textures at delight {}\n", Files::pathToUnicodeString(sheet), drawn.mCount, delight));
    }

    void StopWriter::writeView(const Writing& into, MWRender::OffscreenView& view, const std::filesystem::path& file)
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
        // depth range the settings gave it; what a stop writes is that picture, found by the
        // renderer that drew it as the picture taken straight down over where the player stands.
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        const osg::Vec3f standing = player.getRefData().getPosition().asVec3();
        MWRender::TracedView* tile
            = into.mContext.mRenderer.getViews().findWorldView(osg::Vec2f(standing.x(), standing.y()));

        const osg::Image* drawn = nullptr;
        if (tile != nullptr)
        {
            // The first ask starts the copy; asked again after the drain, it is there.
            tile->keepCopy();
            drawn = tile->getCopy();
            if (drawn == nullptr)
            {
                drawPicturesNow(into.mContext);
                drawn = tile->getCopy();
            }
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
            MWRender::InventoryPreview preview(into.mContext.mRenderer, into.mContext.mResources, subject);
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
        const std::span<const Rtx::TextureRow> rows = scene.textures().getRows();

        // **Found by texture and reported by placement**, because a mesh carries no name of its own
        // once it is a run of triangles: what a walk keeps is the material it arrived wearing, and a
        // material names the file it samples.
        std::uint32_t met = 0;
        for (const Rtx::PlacementRow& row : scene.placements().getRows())
        {
            const Rtx::MeshInstance& instance = row.mInstance;
            if (!instance.isPlaced())
                continue;

            if (instance.mMaterial == Rtx::sNoIndex)
                continue;

            const Rtx::Material& material = scene.materials().getRows()[instance.mMaterial];
            if (material.mDiffuse == Rtx::sNoIndex)
                continue;

            const std::string_view path = rows[material.mDiffuse].mPath.value();
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

    bool StopWriter::checkHolds(const MWRender::FrameContext& context, const MWRender::FrameReport& report,
        const Rtx::Check check, const StopFacts& facts, std::string& found)
    {
        const Rtx::SceneDesc& scene = context.mScene;
        const Rtx::ExtractionStats& stats = report.mWalked.mFound;

        switch (check)
        {
            case Rtx::Check::WalkTwice:
            {
                if (!report.mWalked.mAgain.has_value())
                {
                    found = "no second walk was made";
                    return false;
                }

                const Rtx::ExtractionStats& again = *report.mWalked.mAgain;
                found = std::format(
                    "{} meshes and {} materials added by the second walk, {} drawables resolved, {} stood again",
                    again.mMeshesAdded, again.mMaterialsAdded, again.mMeshesReused, again.mRestood);
                return again.mMeshesAdded == 0 && again.mMaterialsAdded == 0 && again.mMeshesReused > 0
                    && again.mRestood == 0;
            }

            case Rtx::Check::SurfacesDescribed:
                // **The emitters are reported and not asserted**, for the reason
                // `ExtractionStats::mSpritelessEmitters` gives: every world carries one of the
                // rasterizer's that the traced path answers for itself.
                found = std::format("{} surfaces undescribed, {} emitters spriteless", stats.mUndescribedSurfaces,
                    stats.mSpritelessEmitters);
                return stats.mUndescribedSurfaces == 0;

            case Rtx::Check::LightsPlaced:
            {
                const bool indoors = !MWBase::Environment::get().getWorld()->isCellExterior();
                found = std::format("{} lights casting {}", scene.lights().size(),
                    indoors ? "in a room" : "under a sky, where none is a fair answer");
                return !indoors || !scene.lights().empty();
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
                const float reach = context.mReach;
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

            case Rtx::Check::GroundStands:
            {
                // **Every cell of the reach, the active grid's included**: the game builds no ground
                // for this renderer, so a cell short is a hole the player can walk on. The reach is
                // the disc `Rtx::withinReach` draws about the eye the walk stood, counted by the
                // same rule.
                const bool outdoors = MWBase::Environment::get().getWorld()->isCellExterior();
                std::uint32_t expected = 0;
                Rtx::forEachCellWithin(context.mEye, context.mReach, [&](const osg::Vec2i&) { ++expected; });

                found = std::format(
                    "{} cells of ground stand against {} in the reach", stats.mGroundCells, outdoors ? expected : 0);
                return !outdoors || stats.mGroundCells == expected;
            }

            case Rtx::Check::LightsNotDoubled:
            {
                std::vector<osg::Vec3f> where;
                where.reserve(scene.lights().size());
                for (const Rtx::Light& light : scene.lights())
                    where.push_back(light.mPosition);

                // `osg::Vec3f` orders lexicographically already, which is what a sort for
                // duplicates needs and what its own `operator<` promises.
                std::sort(where.begin(), where.end());

                const auto doubled = std::adjacent_find(where.begin(), where.end());
                found = std::format(
                    "{} lights, {}", where.size(), doubled == where.end() ? "no two at one point" : "two at one point");

                return doubled == where.end();
            }

            case Rtx::Check::StaticsNotDoubled:
            {
                // **Asked of the game's registry and not of the walk**, because the walk knows a
                // placement by its node and the ring knows one by its reference: what the two share
                // is the game's own `Ptr`, and a base node on it is the game standing the reference
                // in the graph the walk mirrors.
                const MWWorld::WorldModel& model = *MWBase::Environment::get().getWorldModel();
                std::vector<ESM::RefNum> standing;
                context.mRenderer.collectStanding(standing);

                std::size_t doubled = 0;
                std::string first;
                for (const ESM::RefNum refnum : standing)
                {
                    const MWWorld::Ptr stood = model.getPtr(refnum);
                    if (stood.isEmpty() || stood.getRefData().getBaseNode() == nullptr)
                        continue;

                    if (doubled++ == 0)
                        first = std::format(", the first {} at {}", stood.getCellRef().getRefId().toDebugString(),
                            stood.getCell()->getCell()->getDescription());
                }

                found = std::format(
                    "{} statics the ring stands, {} of them stood by the game too{}", standing.size(), doubled, first);
                return doubled == 0;
            }

            case Rtx::Check::TexturesReadable:
                found = std::format("{} of {} textures could not be read", report.mUnreadableTextures,
                    scene.textures().getRows().size());
                return report.mUnreadableTextures == 0;

            case Rtx::Check::CrossingsAppend:
            {
                const Rtx::Crossings& crossings = facts.mCrossings;
                found = std::format("{} crossings, {} of them rebuilds", crossings.mCount, crossings.mRebuilds);
                return crossings.mCount > 0 && crossings.mRebuilds < crossings.mCount;
            }

            case Rtx::Check::FramesOverlap:
            {
                const Rtx::Overlap& overlap = facts.mOverlap;
                found = std::format(
                    "{:.2f} frames in flight at a submit, {} at the least", overlap.getMean(), overlap.mLeast);
                return overlap.mFrames > 0 && overlap.mLeast == 2;
            }

            case Rtx::Check::QueueHeld:
            {
                const auto zone = std::find_if(facts.mZones.begin(), facts.mZones.end(),
                    [](const Rtx::GpuZone& held) { return held.mName == Rtx::RenderProfile::sHoldZone; });
                if (zone == facts.mZones.end() || zone->mFrames == 0)
                {
                    found = "no frame timed the hold";
                    return false;
                }

                // Every frame held for at least as long as asked, by both clocks: the loop's own,
                // which leaves at its first tick past the time and so cannot read shorter unless
                // the loop is wrong, and the timer's zone around it, which says the queue was
                // occupied for all of it. No bound above, because everything past the tick is
                // the card's — a clock switch, a compositor's slice of the device — and a hold
                // that ran long is a queue held longer, which weakens nothing the hold is for. The
                // longest reading is printed so a stall can be seen for what it is, and the zone
                // beside the loop so its excess is known to be the launch and the drain.
                const Rtx::HoldTimes& hold = facts.mHold;
                const double zoneShortest = zone->mTimes.mBest;
                found = std::format(
                    "{:.3f} ms held by the loop at the shortest frame of {:.1f} asked, {:.3f} at the "
                    "longest, {:.3f} timed around it at the shortest, on {} of {} frames",
                    hold.mShortestMs, facts.mHoldAskedMs, hold.mLongestMs, zoneShortest, zone->mFrames,
                    zone->mOfFrames);
                return zone->mFrames == zone->mOfFrames && hold.mFrames == zone->mOfFrames
                    && hold.mShortestMs >= facts.mHoldAskedMs && zoneShortest >= facts.mHoldAskedMs;
            }

            case Rtx::Check::DriverQuiet:
            {
                const Rtx::ThreadShare& threads = facts.mThreads;
                if (!threads.mViewed)
                {
                    found = "no view of the process's other threads on this platform";
                    return true;
                }

                // A stop shorter than the stretch cannot hold one, and says how short it was so
                // the yes reads as what it is.
                found = std::format(
                    "{} windows, the busiest thread flat out through {} in a row at most, against "
                    "the {} a compile holds",
                    threads.mWindows, threads.mLongestFlatOut, Rtx::ThreadShare::sCompileWindows);
                return threads.mLongestFlatOut < Rtx::ThreadShare::sCompileWindows;
            }

            case Rtx::Check::CameraStands:
            {
                // **Answered rather than compared, where the stop named no camera.** Measuring the
                // camera against itself is a yes nothing could fail, which reads in the report
                // exactly like a camera that held.
                const Rtx::Stand& stand = facts.mStand;
                if (!stand.mEye.has_value())
                {
                    found = "the stop named no camera of its own";
                    return true;
                }

                // **The game's camera and not the note the session took**, which is read off the
                // same object: a check against that would agree with itself however far either had
                // drifted from what the stop asked for.
                const MWRender::Camera& camera
                    = *MWBase::Environment::get().getWorld()->getRenderingManager()->getCamera();
                const osg::Vec3f eye(camera.getPosition());
                const osg::Vec3f forward = camera.getOrient() * osg::Vec3f(0.0f, 1.0f, 0.0f);

                osg::Vec3f asked = stand.getLook() - *stand.mEye;
                asked.normalize();

                // A tenth of a unit and a tenth of a degree: the eye is set from the view file
                // outright, and the aim goes out through a pitch and a yaw and comes back through a
                // quaternion, so what survives is float rounding rather than a tolerance on a
                // measurement.
                const float slipped = (eye - *stand.mEye).length();
                const float turned = osg::RadiansToDegrees(std::acos(std::clamp(forward * asked, -1.0f, 1.0f)));

                found
                    = std::format("the eye stands {:.2f} units and {:.2f}° from what the stop asked", slipped, turned);
                return slipped < 0.1f && turned < 0.1f;
            }
        }

        return false;
    }
}

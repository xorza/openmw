#include "picture.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include <osg/FrameStamp>
#include <osg/Math>
#include <osg/Matrixf>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/debug/debugging.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/fallback/fallback.hpp>
#include <components/files/conversion.hpp>
#include <components/rtx/offscreentrace.hpp>
#include <components/rtx/png.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneuploader.hpp>

#include "actor.hpp"
#include "npc.hpp"
#include "stagedworld.hpp"
#include "world.hpp"

namespace RtxTool
{
    namespace
    {
        /// **Everything, because the one bit the game leaves out is not in reach.** `Mask_*` lives
        /// in `apps/openmw/mwrender/vismask.hpp` and the harness does not link the game; nothing a
        /// harness assembles carries `Mask_UpdateVisitor` either, so the two walks see the same
        /// nodes. A run that wants to reproduce a different mask says so on the command line.
        constexpr osg::Node::NodeMask sEverything = ~0u;

        /// The game's own inventory framing (`MWRender::InventoryPreview`): 512 by 1024, from seven
        /// hundred units in front of the figure at the height of its head.
        constexpr float sDollFieldOfView = 12.3f;
        constexpr float sDollNear = 4.0f;
        constexpr float sDollFar = 10000.0f;

        /// The game's own map framing (`MWRender::LocalMap`).
        constexpr float sMapEyeHeight = 50000.0f;
        constexpr float sMapNear = 5.0f;
        constexpr float sMapFar = 150000.0f;

        /// The light a doll is lit by, as the ini records it. The same arithmetic
        /// `describeInventoryLight` does in the game, from the same four keys.
        void lightAsInventory(Rtx::OffscreenTrace& trace)
        {
            const float azimuth = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationX"));
            const float altitude = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationY"));

            const osg::Vec3f towards(
                -std::cos(azimuth) * std::sin(altitude), std::sin(azimuth) * std::sin(altitude), std::cos(altitude));

            trace.setLight(towards,
                osg::Vec4f(Fallback::Map::getFloat("Inventory_DirectionalDiffuseR"),
                    Fallback::Map::getFloat("Inventory_DirectionalDiffuseG"),
                    Fallback::Map::getFloat("Inventory_DirectionalDiffuseB"), 1.0f),
                osg::Vec4f(Fallback::Map::getFloat("Inventory_DirectionalAmbientR"),
                    Fallback::Map::getFloat("Inventory_DirectionalAmbientG"),
                    Fallback::Map::getFloat("Inventory_DirectionalAmbientB"), 1.0f));
        }

        std::unique_ptr<Rtx::Renderer> makeRenderer(const PictureRequest& request, std::ostream& out)
        {
            std::string reason;
            std::unique_ptr<Rtx::Renderer> renderer = Rtx::createRenderer(
                Rtx::RendererOptions{
                    .mShaderDirectory = request.mShaderDirectory,
                    .mWidth = request.mWidth,
                    .mHeight = request.mHeight,
                },
                reason);

            if (renderer == nullptr)
                out << reason << '\n';

            return renderer;
        }

        /// How much of the picture is something rather than the background it was cleared to.
        ///
        /// **The summary line's whole job, and the same one `shot`'s hit fraction does**: it tells
        /// "the doll rendered" from "the camera faced away from it" without anyone opening the file.
        /// A picture of nothing reads zero, and that is the failure these commands exist to catch.
        double coveredFraction(std::span<const std::uint8_t> pixels, const osg::Vec4f& clear)
        {
            const std::uint8_t background[4] = { static_cast<std::uint8_t>(clear.r() * 255.0f + 0.5f),
                static_cast<std::uint8_t>(clear.g() * 255.0f + 0.5f),
                static_cast<std::uint8_t>(clear.b() * 255.0f + 0.5f),
                static_cast<std::uint8_t>(clear.a() * 255.0f + 0.5f) };

            std::size_t covered = 0;
            for (std::size_t at = 0; at + 3 < pixels.size(); at += 4)
            {
                if (pixels[at] != background[0] || pixels[at + 1] != background[1] || pixels[at + 2] != background[2]
                    || pixels[at + 3] != background[3])
                    ++covered;
            }

            const std::size_t total = pixels.size() / 4;
            return total == 0 ? 0.0 : static_cast<double>(covered) / static_cast<double>(total);
        }

        int writePicture(Rtx::Renderer& renderer, std::uint32_t slot, const PictureRequest& request,
            const osg::Vec4f& clear, std::ostream& out)
        {
            std::vector<std::uint8_t> pixels;
            renderer.readGuiTexture(slot, pixels);

            const double covered = coveredFraction(pixels, clear);

            Rtx::writePng(request.mOutput, request.mWidth, request.mHeight, pixels);

            out << "wrote " << Files::pathToUnicodeString(request.mOutput) << ", " << request.mWidth << 'x'
                << request.mHeight << ", " << static_cast<int>(covered * 100.0 + 0.5) << "% of it drawn\n";

            // A picture of nothing is a failure however cleanly it was written.
            return covered > 0.0 ? 0 : 1;
        }
    }

    int runDoll(World& world, const ESM::NPC& npc, const PictureRequest& request)
    {
        std::ostream& out = Debug::getRawStdout();

        Actor actor(world, buildNpc(world, npc, request.mDressed), osg::Matrixf::identity());
        actor.pose(request.mSeconds, request.mSeconds);

        if (actor.getPosedBones() == 0)
            out << "warning: the keyframes reached none of the skeleton's bones — this is the bind pose.\n";

        const std::unique_ptr<Rtx::Renderer> renderer = makeRenderer(request, out);
        if (renderer == nullptr)
            return 1;

        // Transparent, because a doll is composited over the window behind it — and because that is
        // the one thing about a doll's trace a map tile does not also do.
        const osg::Vec4f clear(0.0f, 0.0f, 0.0f, 0.0f);

        const std::uint32_t slot = renderer->addGuiTexture(request.mWidth, request.mHeight);

        Rtx::OffscreenTrace trace(*renderer, request.mWidth, request.mHeight, actor.getRoot(), sEverything);

        trace.setPerspective(sDollFieldOfView, sDollNear, sDollFar);
        trace.setClearColour(clear);
        lightAsInventory(trace);

        const osg::Vec3f origin = request.mOrigin.value_or(osg::Vec3f(0.0f, 700.0f, 71.0f));
        const osg::Vec3f target = request.mTarget.value_or(osg::Vec3f(0.0f, 0.0f, 71.0f));
        trace.setView(osg::Matrixf::lookAt(origin, target, osg::Vec3f(0.0f, 0.0f, 1.0f)));

        // **Its own clock, and it has to read as a frame that has happened.** The traversal number
        // the walk poses at comes from this, and everything skinned refuses to move for a number it
        // has already seen — zero being the number every one of them starts at.
        osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
        stamp->setFrameNumber(1);
        stamp->setSimulationTime(request.mSeconds);
        stamp->setReferenceTime(request.mSeconds);

        if (!trace.rebuildSubject(*stamp, 1, world.getImageManager()))
        {
            out << "Nothing to render: the figure placed no geometry.\n";
            return 1;
        }

        trace.traceInto(slot);

        return writePicture(*renderer, slot, request, clear, out);
    }

    int runMap(World& world, const ESM::Cell& cell, const StagingRequest& staging, const ActorRequest& actors,
        const PictureRequest& request)
    {
        std::ostream& out = Debug::getRawStdout();

        StagedWorld staged(world, cell, staging, actors);
        Rtx::SceneDesc& scene = staged.getScene();

        if (scene.getPlacedCount() == 0)
        {
            out << "Nothing to render: the cell placed no geometry.\n";
            return 1;
        }

        const std::unique_ptr<Rtx::Renderer> renderer = makeRenderer(request, out);
        if (renderer == nullptr)
            return 1;

        // **Staged and not streamed**: this renders the region once, so every composite has to be
        // finished here rather than drained over frames that will never come.
        Rtx::SceneUploader uploader;
        uploader.setStaged(true);
        uploader.hand(*renderer, Rtx::sWorld, scene, world.getImageManager(), Rtx::SeaState{});

        const osg::Vec4f clear(0.0f, 0.0f, 0.0f, 1.0f);

        const std::uint32_t slot = renderer->addGuiTexture(request.mWidth, request.mHeight);

        Rtx::OffscreenTrace trace(*renderer, request.mWidth, request.mHeight);

        // One cell across, which is what a tile is: the game divides a cell's bounds into this and
        // draws one of these per square.
        const float side = static_cast<float>(ESM::getCellSize(ESM::Cell::sDefaultWorldspaceId));
        trace.setOrthographic(side, side, sMapNear, sMapFar);
        trace.setClearColour(clear);

        // Flat and from nowhere in particular, exactly as `LocalMap::draw` asks for it: a chart is
        // read for what is where, and a sun angle that made shadows would only make it harder.
        trace.setLight(
            osg::Vec3f(-0.3f, -0.3f, 0.7f), osg::Vec4f(0.7f, 0.7f, 0.7f, 1.0f), osg::Vec4f(0.3f, 0.3f, 0.3f, 1.0f));

        // The middle of the cell for an exterior, whose square is known before anything is read;
        // the middle of what was staged for an interior, whose extent is whatever the room is.
        const osg::BoundingBoxf bounds = scene.getBounds();
        const osg::Vec3f middle = cell.isExterior()
            ? osg::Vec3f((cell.getGridX() + 0.5f) * side, (cell.getGridY() + 0.5f) * side, 0.0f)
            : osg::Vec3f(bounds.center().x(), bounds.center().y(), 0.0f);

        trace.setView(osg::Matrixf::lookAt(osg::Vec3f(middle.x(), middle.y(), sMapEyeHeight),
            osg::Vec3f(middle.x(), middle.y(), sMapEyeHeight - 1.0f), osg::Vec3f(0.0f, 1.0f, 0.0f)));

        trace.traceInto(slot);

        return writePicture(*renderer, slot, request, clear, out);
    }
}

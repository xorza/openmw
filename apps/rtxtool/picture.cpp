#include "picture.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include <osg/FrameStamp>
#include <osg/Matrixf>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/debug/debugging.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/files/conversion.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/rtx/offscreentrace.hpp>
#include <components/rtx/png.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneuploader.hpp>
#include <components/sceneutil/offscreenframing.hpp>

#include "actor.hpp"
#include "npc.hpp"
#include "stagedworld.hpp"
#include "world.hpp"

namespace RtxTool
{
    namespace
    {
        /// **The harness's own, where the game's is the segment it is about to draw.** `LocalMap`
        /// fits the camera to each tile's z extent, which it knows because it has just measured it;
        /// this stands above every cell there is and sees far enough to reach the bottom of one.
        constexpr float sMapEyeHeight = 50000.0f;
        constexpr float sMapFar = 150000.0f;

        void lightAs(Rtx::OffscreenTrace& trace, const SceneUtil::FlatLight& light)
        {
            trace.setLight(light.mDirection, light.mDiffuse, light.mAmbient);
        }

        std::unique_ptr<Rtx::Renderer> makeRenderer(
            const PictureRequest& request, const Rtx::ValidationOptions& validation, std::ostream& out)
        {
            std::string reason;
            std::unique_ptr<Rtx::Renderer> renderer = Rtx::createRenderer(request.describeRenderer(validation), reason);

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

    Rtx::RendererOptions PictureRequest::describeRenderer(const Rtx::ValidationOptions& validation) const
    {
        return Rtx::RendererOptions{
            .mShaderDirectory = mShaderDirectory,
            .mCacheDirectory = mCacheDirectory,
            .mWidth = mWidth,
            .mHeight = mHeight,
            .mValidation = validation,
        };
    }

    int runDoll(
        World& world, const ESM::NPC& npc, const Rtx::ValidationOptions& validation, const PictureRequest& request)
    {
        std::ostream& out = Debug::getRawStdout();

        Actor actor(world, buildNpc(world, npc, request.mDressed), osg::Matrixf::identity());
        actor.pose(request.mSeconds, request.mSeconds);

        if (actor.getPosedBones() == 0)
            out << "warning: the keyframes reached none of the skeleton's bones — this is the bind pose.\n";

        const std::unique_ptr<Rtx::Renderer> renderer = makeRenderer(request, validation, out);
        if (renderer == nullptr)
            return 1;

        // Transparent, because a doll is composited over the window behind it — and because that is
        // the one thing about a doll's trace a map tile does not also do.
        const osg::Vec4f clear(0.0f, 0.0f, 0.0f, 0.0f);

        const std::uint32_t slot = renderer->addGuiTexture(request.mWidth, request.mHeight);

        // **Everything the content did not hide**, which is what `Rtx::SceneExtractor` walks the
        // world with and what `World` chose the bit for. The game's other exclusions — sky, sun,
        // simple water — name nodes a doll's subtree does not have.
        Rtx::OffscreenTrace trace(
            *renderer, request.mWidth, request.mHeight, actor.getRoot(), ~NifOsg::Loader::getHiddenNodeMask());

        trace.setPerspective(SceneUtil::sPreviewFieldOfView, SceneUtil::sPreviewNear, SceneUtil::sPreviewFar);
        trace.setClearColour(clear);
        lightAs(trace, SceneUtil::inventoryLight());

        const SceneUtil::PreviewCamera camera = SceneUtil::inventoryCamera();
        const osg::Vec3f origin = request.mOrigin.value_or(camera.mOrigin);
        const osg::Vec3f target = request.mTarget.value_or(camera.mTarget);
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
        const Rtx::ValidationOptions& validation, const PictureRequest& request)
    {
        std::ostream& out = Debug::getRawStdout();

        StagedWorld staged(world, cell, staging, actors);
        Rtx::SceneDesc& scene = staged.getScene();

        if (scene.getPlacedCount() == 0)
        {
            out << "Nothing to render: the cell placed no geometry.\n";
            return 1;
        }

        const std::unique_ptr<Rtx::Renderer> renderer = makeRenderer(request, validation, out);
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
        trace.setOrthographic(side, side, SceneUtil::sMapNear, sMapFar);
        trace.setClearColour(clear);
        lightAs(trace, SceneUtil::mapLight());

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

#include "verify.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <components/debug/debugging.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/files/conversion.hpp>
#include <components/rtx/renderer.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneuploader.hpp>
#include <components/rtx/wavespectrum.hpp>

#include "content.hpp"
#include "framing.hpp"
#include "settling.hpp"
#include "stagedworld.hpp"
#include "world.hpp"

namespace RtxTool
{
    namespace
    {
        std::ostream& out()
        {
            return Debug::getRawStdout();
        }

        /// Where a view's frame is written, under whichever run directory.
        std::filesystem::path frameFile(const std::filesystem::path& directory, const std::string& view)
        {
            return directory / (view + ".png");
        }

        /// Where the picture a view drew before it settled goes, when it is not the one after.
        std::filesystem::path earlyFile(const std::filesystem::path& directory, const std::string& view)
        {
            return directory / (view + ".early.png");
        }

        /// How a difference reads on one line.
        std::string describe(const FrameDifference& difference)
        {
            if (difference.mMismatched)
                return "no reference, or one of a different size";

            if (difference.same())
                return "same";

            return std::format(
                "differs: worst {} of 255 on {:.2f}% of the pixels", difference.mWorst, difference.getPercent());
        }
    }

    double FrameDifference::getPercent() const
    {
        return mTotal == 0 ? 0.0 : static_cast<double>(mDiffering) / static_cast<double>(mTotal) * 100.0;
    }

    FrameDifference closestDifference(const Rtx::PngImage& taken, std::span<const Rtx::PngImage> references)
    {
        FrameDifference closest{ .mMismatched = true };
        for (const Rtx::PngImage& reference : references)
        {
            const FrameDifference difference = compareFrames(reference, taken);
            if (difference.same())
                return difference;

            if (!difference.mMismatched && (closest.mMismatched || difference.mDiffering < closest.mDiffering))
                closest = difference;
        }

        return closest;
    }

    FrameDifference compareFrames(const Rtx::PngImage& before, const Rtx::PngImage& after)
    {
        if (before.empty() || after.empty() || before.mWidth != after.mWidth || before.mHeight != after.mHeight)
            return FrameDifference{ .mMismatched = true };

        FrameDifference difference;
        difference.mTotal = std::uint64_t{ before.mWidth } * before.mHeight;

        for (std::size_t at = 0; at + 3 < before.mPixels.size(); at += 4)
        {
            std::uint32_t worst = 0;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const auto one = static_cast<std::int32_t>(before.mPixels[at + channel]);
                const auto other = static_cast<std::int32_t>(after.mPixels[at + channel]);
                worst = std::max(worst, static_cast<std::uint32_t>(std::abs(one - other)));
            }

            if (worst > 0)
            {
                ++difference.mDiffering;
                difference.mWorst = std::max(difference.mWorst, worst);
            }
        }

        return difference;
    }

    int runVerify(World& world, const Rtx::ValidationOptions& validation, const VerifyRequest& request)
    {
        std::filesystem::create_directories(request.mOut);

        // **Upscaling off, and not offered as an option.** Ray Reconstruction is temporal and
        // carries state the code below cannot hold still: two builds that describe the same scene
        // identically write different bytes through it, and fifteen of sixteen views once read as
        // changed by a refactor that changed nothing. One renderer for the whole run, for the
        // reason `bench` gives.
        Rtx::RendererOptions options = request.mFrame.describeRenderer(validation);
        options.mUpscale = Rtx::Upscale::Off;

        std::string reason;
        const std::unique_ptr<Rtx::Renderer> renderer = Rtx::createRenderer(options, reason);
        if (renderer == nullptr)
        {
            out() << reason << '\n';
            return 1;
        }

        const Rtx::FrameExtents extents = renderer->getExtents();

        out() << std::format("verify: {} {} at {}x{}, upscaling off\n", request.mViews.size(),
            request.mViews.size() == 1 ? "view" : "views", extents.mOutputWidth, extents.mOutputHeight);

        if (request.mAgainst.empty())
            out() << "        no --against, so this run is only a reference for the next one\n";

        std::uint32_t differing = 0;
        std::uint32_t unmatched = 0;

        for (const View& view : request.mViews)
        {
            const ESM::Cell* cell = world.getContent().findCell(view.mCell);
            if (cell == nullptr)
            {
                out() << std::format("  {:<28} no cell called \"{}\"\n", view.mName, view.mCell);
                return 1;
            }

            StagedWorld staged(world, *cell, request.mFrame.describeStaging(view), request.mFrame.mActors);

            if (staged.empty())
            {
                out() << std::format("  {:<28} the region placed no geometry\n", view.mName);
                return 1;
            }

            // **Staged and not streamed**: this renders the region once, so every composite has to be
            // finished here rather than drained over frames that will never come.
            Rtx::SceneUploader uploader;
            uploader.setStaged(true);
            uploader.hand(*renderer, Rtx::sWorld, staged.getScene(), world.getImageManager(), Rtx::SeaState{});

            Framing framing = Framing::lookingFrom(staged.getPlacement());
            framing.mFieldOfView = request.mFrame.mFieldOfView;
            framing.mLighting = staged.getLighting();
            framing.mDelight = request.mFrame.mDelight;
            framing.mShowAlbedo = request.mFrame.mShowAlbedo;

            // **One frame at seed zero.** Everything a repeat buys is a timing figure, and this
            // command measures nothing; a second frame would only give the sampler somewhere else
            // to be. The one frame is drawn again where `watchSettling` says so.
            framing.mFrame = 0;

            // **Each drawing is a first frame of a place staged into a shared renderer, which is a
            // discontinuity**, and the exposure is what would otherwise carry across one: it adapts
            // toward its measurement over seconds rather than taking it, so a room drawn after a
            // noon exterior opens at the exterior's brightness, and a view drawn twice would open
            // the second time on its own first. `Rtx::Renderer::resetHistory` says why only a caller
            // can know this.
            const auto draw = [&](std::vector<std::uint8_t>& pixels) {
                renderer->resetHistory();
                renderer->renderFrame(makeFrameConstants(framing, extents),
                    Rtx::FrameOptions{ .mSinceLast = sStepSeconds,
                        .mExposureBias = framing.mLighting.mDaylight.mExposureBias,
                        .mFilter = request.mFrame.mFilter,
                        .mExposure = request.mFrame.mExposure });
                renderer->readPixels(pixels);
            };

            Rtx::PngImage early{ extents.mOutputWidth, extents.mOutputHeight, {} };
            draw(early.mPixels);

            // The settled picture a previous run kept, and the early one where its two differed;
            // `readPng` is empty for whichever was never written, and empty is passed over.
            std::array<Rtx::PngImage, 2> references;
            if (!request.mAgainst.empty())
                references = { Rtx::readPng(frameFile(request.mAgainst, view.mName)),
                    Rtx::readPng(earlyFile(request.mAgainst, view.mName)) };

            FrameDifference difference = closestDifference(early, references);

            // **A reference keeps both pictures, so a run is compared with whichever it drew.** A
            // reference watches every view for the settling `watchSettling` describes; a run
            // watches only a view whose first picture matched nothing the reference holds, which is
            // what keeps the wait off every run that lands on a side the reference knows.
            Rtx::PngImage settled{ extents.mOutputWidth, extents.mOutputHeight, {} };
            std::optional<double> settledAt;
            const bool watched = request.mAgainst.empty() || !difference.same();
            if (watched)
            {
                settledAt = watchSettling(draw, early.mPixels, settled.mPixels);
                if (!request.mAgainst.empty())
                    difference = closestDifference(settled, references);
            }

            const Rtx::PngImage& last = watched ? settled : early;
            Rtx::writePng(frameFile(request.mOut, view.mName), last.mWidth, last.mHeight, last.mPixels);
            if (settledAt.has_value())
                Rtx::writePng(earlyFile(request.mOut, view.mName), early.mWidth, early.mHeight, early.mPixels);

            const std::string settling = watched ? describeSettling(settledAt) : std::string();

            if (request.mAgainst.empty())
            {
                out() << std::format("  {:<28} {}\n", view.mName, settling);
                continue;
            }

            unmatched += difference.mMismatched ? 1u : 0u;
            differing += !difference.mMismatched && !difference.same() ? 1u : 0u;

            out() << std::format("  {:<28} {}{}{}\n", view.mName, describe(difference), watched ? ", " : "", settling);
        }

        out() << std::format("wrote {} to {}\n", request.mViews.size() == 1 ? "1 frame" : "frames",
            Files::pathToUnicodeString(request.mOut));

        if (request.mAgainst.empty())
            return 0;

        out() << std::format("{} {}, {} differ, {} without a reference\n", request.mViews.size(),
            request.mViews.size() == 1 ? "view" : "views", differing, unmatched);

        return differing + unmatched > 0 ? 1 : 0;
    }
}

#include "verify.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <memory>
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

#include "framing.hpp"
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

        std::string reason;
        // **Upscaling off, and not offered as an option.** Ray Reconstruction is temporal and
        // carries state the code below cannot hold still: two builds that describe the same scene
        // identically write different bytes through it, and fifteen of sixteen views once read as
        // changed by a refactor that changed nothing. One renderer for the whole run, for the
        // reason `bench` gives.
        const std::unique_ptr<Rtx::Renderer> renderer = Rtx::createRenderer(
            Rtx::RendererOptions{
                .mShaderDirectory = request.mShaderDirectory,
                .mWidth = request.mWidth,
                .mHeight = request.mHeight,
                .mUpscale = Rtx::Upscale::Off,
                .mValidation = validation,
            },
            reason);
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
            const ESM::Cell* cell = world.findCell(view.mCell);
            if (cell == nullptr)
            {
                out() << std::format("  {:<28} no cell called \"{}\"\n", view.mName, view.mCell);
                return 1;
            }

            StagedWorld staged(world, *cell,
                StagingRequest{
                    .mWeather = request.mWeather,
                    .mHour = request.mHour,
                    .mDay = request.mDay,
                    .mFieldOfView = request.mFieldOfView,
                    .mOrigin = view.mOrigin,
                    .mTarget = view.mTarget,
                },
                request.mActors);

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
            framing.mFieldOfView = request.mFieldOfView;
            framing.mFar = std::max(staged.getScene().getBounds().radius() * 8.0f, 10000.0f);
            framing.mLighting = staged.getLighting();
            framing.mDelight = request.mDelight;

            // **One frame at seed zero.** Everything a repeat buys is a timing figure, and this
            // command measures nothing; a second frame would only give the sampler somewhere else
            // to be.
            framing.mFrame = 0;

            renderer->renderFrame(makeFrameConstants(framing, extents),
                Rtx::FrameOptions{ .mFilter = request.mFilter, .mExposure = request.mExposure });

            std::vector<std::uint8_t> pixels;
            renderer->readPixels(pixels);
            Rtx::writePng(frameFile(request.mOut, view.mName), extents.mOutputWidth, extents.mOutputHeight, pixels);

            if (request.mAgainst.empty())
                continue;

            const Rtx::PngImage reference = Rtx::readPng(frameFile(request.mAgainst, view.mName));
            const Rtx::PngImage taken{ extents.mOutputWidth, extents.mOutputHeight, std::move(pixels) };
            const FrameDifference difference = compareFrames(reference, taken);

            unmatched += difference.mMismatched ? 1u : 0u;
            differing += !difference.mMismatched && !difference.same() ? 1u : 0u;

            out() << std::format("  {:<28} {}\n", view.mName, describe(difference));
        }

        out() << std::format("wrote {} to {}\n", request.mViews.size() == 1 ? "1 frame" : "frames",
            Files::pathToUnicodeString(request.mOut));

        if (request.mAgainst.empty())
            return 0;

        out() << std::format(
            "{} views, {} differ, {} without a reference\n", request.mViews.size(), differing, unmatched);

        return differing + unmatched > 0 ? 1 : 0;
    }
}

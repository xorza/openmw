#include "measurer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <apps/openmw/mwbase/environment.hpp>
#include <apps/openmw/mwbase/world.hpp>
#include <apps/openmw/mwrender/rtx/framereport.hpp>
#include <apps/openmw/mwrender/rtx/rtxrenderer.hpp>
#include <apps/openmw/mwworld/ptr.hpp>
#include <apps/openmw/mwworld/timestamp.hpp>
#include <components/debug/debuglog.hpp>
#include <components/rtx/contract.hpp>
#include <components/rtx/framespend.hpp>
#include <components/rtx/skylight.hpp>
#include <components/rtx/texels.hpp>
#include <components/rtxbench/benchspec.hpp>
#include <components/rtxbench/framehashes.hpp>
#include <components/rtxbench/gpuclock.hpp>

#include "film.hpp"
#include "model/benchrun.hpp"
#include "model/runrecord.hpp"
#include "stopwriter.hpp"

namespace RtxTool
{
    Measurer::Measurer(const SessionRequest& request, RunRecord& record)
        : mRequest(request)
        , mRecord(record)
        , mProfiling(request.mPerfControl)
    {
        // Reserved once at the longest stop's length, so no measured frame grows a vector — a
        // benchmark that stops to reallocate is measuring its own allocator. A window that runs
        // until it is closed is not a benchmark, and room for its "length" is thirty gigabytes a
        // row: Linux promised that and never gave it, and Windows refuses it outright.
        std::uint32_t longest = 0;
        for (const Stop& stop : request.mStops)
            if (!stop.mSchedule.mSpec.mRun.isUntilClosed())
                longest = std::max(longest, stop.mSchedule.mSpec.getMeasured(request.mSetup.getWorldStep()));

        mProgress.mSamples.reserve(longest);
        mProgress.mLatencyMs.reserve(longest);
        mProgress.mGpu.reserve(longest);

        // **From here and not from the first frame**, because the window before the first stop
        // is the load, at the card's idle clock, where a desktop that is drawing shows plainest;
        // `Rtx::CardWatch` says why.
        mCardWatch.watch();
    }

    void Measurer::begin(const Stop& stop)
    {
        // **First, because everything below writes into it.** A stop's progress is one object so
        // that a field added to it is reset here whether or not its author remembered to.
        mProgress.restart();

        mProgress.mCell = MWBase::Environment::get().getWorld()->getPlayerPtr().getCell();
        mProgress.mPlace.mView = stop.mName;
        mProgress.mPlace.mCell = stop.mStand.mCell;
        mProgress.mPlace.mNote = stop.mNote;
    }

    Measurer::Verdict Measurer::frame(const Stop& stop, const MWRender::FrameContext& context,
        const MWRender::FrameReport& report, const bool arrived)
    {
        Rtx::Renderer& renderer = context.mRenderer.getBackend();
        const double frameMs = report.mSpend.at(Rtx::Timing::Frame);
        const float step = mRequest.mSetup.getWorldStep();
        const std::uint32_t warmup = stop.mSchedule.mSpec.getWarmup(step);
        const std::uint32_t measured = stop.mSchedule.mSpec.getMeasured(step);

        if (mProgress.mSeen == warmup)
        {
            // **Sampled through the measured frames and not at their ends.** Two readings bound
            // nothing: the ends of a place agree to within a couple of per cent while the card
            // moves a fifth of its clock between them, and a leg that lost its clock then reads
            // like a leg that lost its speed.
            //
            // **What the window before answered is said once, ahead of the first place.** A
            // desktop that was drawing while the run loaded was caught in nearly every sample,
            // and every place's own line then reads against it.
            const Rtx::CardShare before = mCardWatch.start();
            if (mRecord.empty() && before.mViewed)
                mRecord.note(std::format("before the first stop, {}\n", Rtx::describeCard(before)));

            mProfiling.enable();

            // The backend's number of the first measured frame: what says of a result that comes
            // back later whether the frame it answers for was measured.
            mProgress.mFirstMeasured = report.mFrame;
        }

        // **Counted here, at the frame the run traced, and never at the frame the device answered
        // for it.** The count is what the trace's sampler and the upscaler's jitter are walked by,
        // what the hashes table numbers its rows by and what ends the stop; and a result comes back
        // one frame later or two, by whether the card had finished when the frame after asked —
        // so a count of results put two runs of one build at different points of the sequence,
        // and paired the scene of one frame with the picture of another. What the device answered
        // is taken in below, under the number of the frame it answers for.
        ++mProgress.mSeen;

        if (report.mResult.has_value())
            answered(stop, *report.mResult, renderer.getExtents());
        if (!mFailure.empty())
            return Verdict::Failed;

        // **This frame's wall time closes the span the frame before it worked in.**
        // `Timing::Frame` runs from the last frame's opening to this one's, so what it
        // holds is the game's update this frame arrived through and the renderer's work of the
        // frame before — the finish, the walk, the placement and the trace that ran after that
        // frame opened. Those are the rows kept beside it, and the meshes that work brought, so a
        // row's figures are the figures of the span it is read against and a worst frame's shares
        // are its own. What this frame does is kept for the frame after to close, and the last
        // measured frame's work is closed by nothing: it ran after the last span ended.
        Rtx::FrameSpend closed = mProgress.mPendingSpend;
        closed.at(Rtx::Timing::Frame) = frameMs;
        closed.at(Rtx::Timing::Update) = report.mSpend.at(Rtx::Timing::Update);
        closed.at(Rtx::Timing::Sleep) = report.mSpend.at(Rtx::Timing::Sleep);
        const std::uint32_t closedArrived = mProgress.mPendingArrived;
        mProgress.mPendingSpend = report.mSpend;
        mProgress.mPendingArrived = report.mArrivedMeshes;

        // A window that runs until it is closed is looked at and not measured: nothing reads its
        // figures, and every series kept for it grew for as long as the window stood open.
        if (mProgress.mSeen <= warmup || stop.mSchedule.mSpec.mRun.isUntilClosed())
            return Verdict::Going;

        mProgress.mSamples.add(closed);
        mProgress.mPlace.mArrivals.add(closedArrived, closed);
        mProgress.mWallMs += frameMs;

        // The driver's own figure of the newest frame it finished, kept where there is one: a
        // series of its own and not a row, because it is not a stretch of the host's frame.
        if (report.mLatency.has_value())
            mProgress.mLatencyMs.push_back(static_cast<double>(report.mLatency->mInputToPresentUs) / 1000.0);

        // **Counted here and not where the route moved**, because a crossing is a dropped frame and
        // this is where what it dropped is known. The move pulls the next ring in and that read
        // lands in the frame after it — which is the frame that arrives here standing in a cell it
        // was not drawn in last time, and the frame that paid for it.
        //
        // **The whole frame goes in as the read**, because the game gives no split: the ring
        // arrives on the loading threads, and what a crossing costs here is the frame that dropped.
        if (const void* cell = MWBase::Environment::get().getWorld()->getPlayerPtr().getCell();
            mProgress.mCell != nullptr && cell != mProgress.mCell)
        {
            mProgress.mPlace.mCrossings.add(report.mRebuilt, frameMs);
            mProgress.mCell = cell;
        }

        const std::uint32_t drawn = mProgress.mSeen - warmup;

        // Numbered here, where the frame is traced, for `writeFilmFrame` to find when its picture
        // comes back.
        if (const std::optional<Actions::Film>& film = stop.mActions.mFilm; film.has_value())
        {
            Rtx::contract(mProgress.mFilmPending < mProgress.mFilmFrames.size(),
                "more of a film's frames in flight than the ring holds");
            mProgress.mFilmFrames[mProgress.mFilmPending++]
                = Progress::FilmFrame{ .mFrame = report.mFrame, .mNumber = film->mFirst + drawn - 1 };
        }

        // The scene of this frame under the number the backend gave the frame, which is what the
        // picture finds its row by when it comes back. A frame the warm-up drew has no row.
        if (stop.mActions.mHash)
            mRecord.getHashes().note(
                stop.mName, drawn, report.mFrame, mDigester.digest(context.mScene, &report.mConstants));

        return drawn < measured && !arrived ? Verdict::Going : Verdict::Ended;
    }

    void Measurer::answered(const Stop& stop, const Rtx::FrameResult& finished, const Rtx::FrameExtents& extents)
    {
        // A frame the warm-up drew: its picture has no row and its figures are nobody's.
        const Rtx::BenchSpec& spec = stop.mSchedule.mSpec;
        if (mProgress.mSeen <= spec.getWarmup(mRequest.mSetup.getWorldStep()) || spec.mRun.isUntilClosed()
            || finished.mFrame < mProgress.mFirstMeasured)
            return;

        mProgress.mPlace.mOverlap.add(finished.mInFlight);
        mProgress.mGpu.add(finished.mGpu.spans());
        mProgress.mNotFinite.add(finished.mNotFinite);

        // A frame that held, and no other: a run with no hold has no reading to summarise, and a
        // loop that left nothing behind is a frame `QueueHeld` counts as unheld.
        if (finished.mHeldMs > 0.0)
            mProgress.mHold.add(finished.mHeldMs);

        const double traced = static_cast<double>(extents.mRenderWidth) * extents.mRenderHeight;
        if (traced > 0.0)
            mProgress.mPlace.mHitPercent = static_cast<double>(finished.mHits) / traced * 100.0;

        if (stop.mActions.mHash)
            keepPicture(finished, extents);

        if (stop.mActions.mFilm.has_value())
            writeFilmFrame(stop, finished, extents);
    }

    void Measurer::writeFilmFrame(const Stop& stop, const Rtx::FrameResult& finished, const Rtx::FrameExtents& extents)
    {
        std::array<Progress::FilmFrame, 4>& pending = mProgress.mFilmFrames;
        const auto end = pending.begin() + static_cast<std::ptrdiff_t>(mProgress.mFilmPending);
        const auto found = std::find_if(
            pending.begin(), end, [&](const Progress::FilmFrame& one) { return one.mFrame == finished.mFrame; });
        Rtx::contract(found != end, "a film's measured frame came back that `frame` never numbered");

        const std::uint32_t number = found->mNumber;
        std::copy(found + 1, end, found);
        --mProgress.mFilmPending;

        // **A film with a frame missing is a film cut short**: the encoder reads the sequence up to
        // its first gap, so a picture that did not come back ends the run rather than leaving one.
        if (finished.mPixels.empty())
        {
            mFailure = std::format("frame {} of the film came back without its picture", number);
            return;
        }

        Rtx::writePng(stop.mActions.mFilm->mDirectory / frameName(number), extents.mOutputWidth, extents.mOutputHeight,
            finished.mPixels);
    }

    void Measurer::keepPicture(const Rtx::FrameResult& finished, const Rtx::FrameExtents& extents)
    {
        if (finished.mPixels.empty())
            return;

        const std::optional<Rtx::FrameHashes::Pictured> row = mRecord.getHashes().picture(finished);
        if (!row.has_value() || mRequest.mPictures.empty())
            return;

        std::filesystem::create_directories(mRequest.mPictures);
        Rtx::writePng(mRequest.mPictures / std::format("{}-{}.png", row->mView, row->mFrame), extents.mOutputWidth,
            extents.mOutputHeight, finished.mPixels);
    }

    BenchPlace Measurer::finish(const Stop& stop, const MWRender::FrameContext& context,
        const MWRender::FrameReport& report, const float travelled, StopWriter& writer)
    {
        Rtx::Renderer& renderer = context.mRenderer.getBackend();
        const float step = mRequest.mSetup.getWorldStep();

        mProfiling.disable();

        const Rtx::CardReading card = mCardWatch.stop();
        BenchPlace& place = mProgress.mPlace;
        place.mClock = card.mClock;
        place.mCard = card.mShare;

        const Rtx::FrameExtents extents = renderer.getExtents();

        // The last frames' answers are still on the queue: waited out here, where a drain is a
        // stop's to pay and never a frame's, so every measured frame's figures and picture are in
        // the place they belong to.
        while (const std::optional<Rtx::FrameResult> finished = renderer.finishFrame())
            answered(stop, *finished, extents);

        // **A still is one frame traced again, and its depth and motion cannot move unless the
        // code under them did.** Asked of every hashed still that nothing jittered and nothing
        // flew. The build pins the float arithmetic the driver's second code could otherwise take
        // apart (`Rtx::pinFloatArithmetic`), so a frame where either moved is a swapped code
        // computing one of the operations left to the device — a division, a root, a
        // transcendental — otherwise, and the stop's frames are then two codes' and no reference.
        if (stop.mSchedule.mFrozen && !stop.mSchedule.mRoute.has_value() && stop.mActions.mHash
            && !report.mReconstruction.mJitter)
            if (const std::optional<std::uint32_t> moved = mRecord.getHashes().findStillMoved(stop.mName))
            {
                const std::string why = std::format(
                    "the driver's code changed during {}: depth or motion moved at frame {}", stop.mName, *moved);
                Log(Debug::Error) << "Ray tracing session: " << why;
                mRecord.note(why + '\n');
                mRecord.fail();
            }

        if (mRecord.empty())
        {
            // **Taken at the first stop, because every stop of a run is traced by one renderer.**
            // What the record's header states is the configuration the whole run stood under. The
            // upscaling is the frame's own answer — the pair the renderer resolved this frame — and
            // not the renderer's mode alone.
            BenchHeader& header = mRecord.getHeader();
            header.mExtents = extents;
            header.mUpscaling = report.mReconstruction.mUpscaling;
            header.mNoise = report.mReconstruction.mNoise;
            header.mLevelBias = report.mReconstruction.mLevelBias;
            header.mReorder = renderer.getProfile().mReorder;
            header.mValidating = renderer.isValidating();
            header.mMeasured = stop.mSchedule.mSpec.getMeasured(step);
            header.mWarmup = stop.mSchedule.mSpec.getWarmup(step);
        }

        // Summarised ahead of the writer, whose checks read the zones, and kept for the place.
        const std::span<const Rtx::GpuZone> zones = mProgress.mGpu.summariseZones();

        writer.write(context, report, stop.mActions,
            StopFacts{
                .mSamples = mProgress.mSamples,
                .mCrossings = place.mCrossings,
                .mStand = stop.mStand,
                .mOverlap = place.mOverlap,
                .mZones = zones,
                .mHold = mProgress.mHold,
                .mHoldAskedMs = mRequest.mSetup.mProfile.mStressOverlapMs,
                .mNotFinite = mProgress.mNotFinite,
            },
            mRecord);

        // **The hour and the sky of one moment, the stop's last frame**, so a stop the sky turned
        // over, or a film's take that crossed from one weather to another, is not reported at its
        // closing hour under its opening sky.
        const MWBase::World& world = *MWBase::Environment::get().getWorld();
        place.mHour = world.getTimeStamp().getHour();
        place.mWeather = Rtx::weatherName(static_cast<std::uint32_t>(world.getCurrentWeatherScriptId()));
        place.mFrames = mProgress.mSamples.size();
        place.mWallSeconds = mProgress.mWallMs / 1000.0;
        for (std::size_t at = 0; at < Rtx::sTimingCount; ++at)
            place.mRows[at] = Rtx::summarise(mProgress.mSamples.mRows[at]);
        if (!mProgress.mLatencyMs.empty())
            place.mLatency = Rtx::summarise(mProgress.mLatencyMs);
        place.mTravelled = travelled;
        place.mScene = renderer.getSceneStats();
        place.mMemory = renderer.getMemoryReport();
        place.mGpu.assign(zones.begin(), zones.end());

        return std::move(place);
    }
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "benchrecord.hpp"
#include "benchrun.hpp"
#include "framehashes.hpp"

namespace Rtx
{
    /// What a run comes to as it happens: the places measured, the header they stood under, the
    /// report as it is built, and whether anything failed.
    ///
    /// **One type, because a run has one verdict and one account of itself.** The report text, the
    /// exit status, the check tally and the hashes were seven members of whatever drove the run, and
    /// every writer that had something to say reached into all of them — so a writer that wrote a
    /// line and forgot the status reported a failure as a success.
    ///
    /// **Here rather than beside the driver**, because none of it needs a world: a place is
    /// `BenchPlace`, a header is `BenchHeader`, and closing the run is those two written to a file.
    class RunRecord
    {
    public:
        void reserve(std::size_t places) { mPlaces.reserve(places); }

        /// Whether no place has been measured yet, which is what says the header is still to be
        /// taken.
        bool empty() const { return mPlaces.empty(); }

        /// The header, for the one caller that fills it: what the whole run stood under is settled
        /// at the first place, because every place of a run is traced by one renderer.
        BenchHeader& getHeader() { return mHeader; }

        /// Takes one measured place, and prints it into the report.
        void add(BenchPlace place);

        /// Adds to the report.
        void note(std::string_view text) { mReport += text; }

        /// Says the run failed, whatever else it wrote.
        ///
        /// **Apart from `note`, because a line and a verdict are two statements.** A picture that
        /// could not be written says so in the report and fails the run; a picture that was written
        /// says so and does not.
        void fail() { mExitStatus = 1; }

        /// Counts one check and whether it held.
        void checked(bool held);

        /// The hashes this run took, and the ones it is being compared against.
        ///
        /// **The run's own record of what it drew.** A hash is one line of the report at the end and
        /// one number per measured frame until then, so it settles here rather than beside the
        /// frames it was taken from.
        FrameHashes& getHashes() { return mHashes; }
        void readReference(const std::filesystem::path& path) { mReference = FrameHashes::read(path); }

        std::span<const BenchPlace> getPlaces() const { return mPlaces; }
        const std::string& getReport() const { return mReport; }
        int getExitStatus() const { return mExitStatus; }

        /// Closes the run: the total under the places, the check tally, the hashes and the record.
        ///
        /// **Takes the request rather than four paths**, because what it writes and what it compares
        /// against is what the run was asked for — and the request is this component's own.
        void finish(const SessionRequest& request);

        /// Everything a launcher reads back, with `left` where the eye was, or null where the run
        /// reached no place.
        ///
        /// **The whole result and never a field at a time.** A launcher reads four of these and a
        /// run fills all four, so a hand-over written out member by member loses whichever ones
        /// nobody remembered — silently, since an unfilled `SessionResult` is a valid one describing
        /// a camera at the origin.
        SessionResult describe(const Standing* left) const;

    private:
        std::vector<BenchPlace> mPlaces;
        BenchHeader mHeader;

        /// The report as it is built, so a launcher gets the whole of it rather than the log's
        /// timestamped halves.
        std::string mReport;

        int mExitStatus = 0;

        /// How many checks the run asked and how many of them failed.
        std::uint32_t mChecked = 0;
        std::uint32_t mFailed = 0;

        FrameHashes mHashes;
        FrameHashes mReference;
    };
}

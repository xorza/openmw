#include "runrecord.hpp"

#include <format>
#include <utility>

#include <components/files/conversion.hpp>

namespace Rtx
{
    void RunRecord::add(BenchPlace place)
    {
        mPlaces.push_back(std::move(place));
        mReport += describePlace(mPlaces.back());
    }

    void RunRecord::checked(const bool held)
    {
        ++mChecked;
        mFailed += held ? 0u : 1u;
        if (!held)
            fail();
    }

    SessionResult RunRecord::describe(const Standing* const left) const
    {
        SessionResult result;
        result.mExitStatus = mExitStatus;
        result.mPlaces = mPlaces;
        result.mReport = mReport;
        if (left != nullptr)
            result.mLeft = *left;

        return result;
    }

    void RunRecord::finish(const SessionRequest& request)
    {
        mReport += describeTotal(mPlaces, false);

        // **What a `check` run came to, in one line.** A suite asks every check at each of several
        // places, so the verdict is otherwise something a reader counts by hand.
        if (mChecked > 0)
            mReport += std::format("\n{} checks asked, {} failed\n", mChecked, mFailed);

        if (!request.mHashes.empty())
        {
            mHashes.write(request.mHashes);
            mReport += std::format(
                "\nwrote {} frame hashes to {}\n", mHashes.frameCount(), Files::pathToUnicodeString(request.mHashes));
        }

        if (!request.mAgainst.empty())
        {
            mReport += std::format("\nagainst {}\n", Files::pathToUnicodeString(request.mAgainst));
            for (const FrameHashes::ViewDifference& difference : mHashes.against(mReference))
            {
                mReport += std::format("  {:<28} {}\n", difference.mView, describeDifference(difference));
                if (!difference.same())
                    fail();
            }
        }

        if (!request.mJson.empty())
        {
            mHeader.mSuite = request.mSuite;
            writeJson(request.mJson, mHeader, mPlaces);
            mReport += "wrote " + Files::pathToUnicodeString(request.mJson) + '\n';
        }
    }
}

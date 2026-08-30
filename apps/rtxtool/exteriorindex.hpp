#pragma once

#include <map>
#include <utility>

namespace ESM
{
    struct Cell;
}

namespace EsmLoader
{
    struct EsmData;
}

namespace RtxTool
{
    /// Every exterior cell the content files hold, by grid position.
    ///
    /// **Because the files are a flat list of two thousand cells with no order to bisect**, and the
    /// harness asks where a square is over and over: nine squares on every crossing, one more per
    /// cell that departed, and one per square of every chunk of distant land. Scanning the list for
    /// each of them is the same question asked a thousand times.
    ///
    /// Holds pointers into the content, so it must not outlive it.
    class ExteriorIndex
    {
    public:
        explicit ExteriorIndex(const EsmLoader::EsmData& content);

        /// The cell at that grid position, or null where the content has none.
        const ESM::Cell* find(int x, int y) const;

    private:
        std::map<std::pair<int, int>, const ESM::Cell*> mCells;
    };
}

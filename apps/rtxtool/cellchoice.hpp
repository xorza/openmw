#pragma once

#include <string>

namespace ESM
{
    struct Cell;
}

namespace RtxTool
{
    class World;

    /// The cell a command was pointed at, or null with the complaint already printed.
    ///
    /// **Every command starts here**, which is why it is not any one of their business: the
    /// complaint has to read the same whichever of them could not find the cell.
    const ESM::Cell* findCellOrComplain(World& world, const std::string& cellSpec);

    /// Which cell a report is about, and the two things about it nothing else says.
    ///
    /// Its water level, and — for an interior — the air, because only an interior carries an `AMBI`.
    /// An exterior's comes off the weather and the clock, which the cell record knows nothing about.
    void printCellHeading(const ESM::Cell& cell);
}

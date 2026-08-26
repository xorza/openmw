#include "cellchoice.hpp"

#include <ostream>

#include <components/debug/debugging.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/rtx/fogbuilder.hpp>

#include "world.hpp"

namespace RtxTool
{
    namespace
    {
        /// Reports go to the unprefixed stream: `Debug::wrapApplication` stamps a time and a level
        /// on every line of `std::cout`, which is right for a game and wrong for output meant to be
        /// read, diffed or parsed.
        std::ostream& out()
        {
            return Debug::getRawStdout();
        }
    }

    const ESM::Cell* findCellOrComplain(World& world, const std::string& cellSpec)
    {
        const ESM::Cell* cell = world.findCell(cellSpec);
        if (cell == nullptr)
            out() << "No cell is called \"" << cellSpec << "\".\n";

        return cell;
    }

    void printCellHeading(const ESM::Cell& cell)
    {
        out() << "cell:        " << (cell.isExterior() ? "exterior " : "interior ") << '"' << cell.mName << '"';
        if (cell.isExterior())
            out() << " at " << cell.getGridX() << ',' << cell.getGridY();
        out() << "\nwater:       " << (cell.hasWater() ? "yes, at z = " + std::to_string(cell.mWater) : "no") << '\n';

        // Only interiors carry an `AMBI`; an exterior's air comes off the weather and the clock,
        // which the cell says nothing about.
        if (!cell.isExterior())
            out() << "fog:         depth " << cell.mAmbi.mFogDensity << ", extinction "
                  << Rtx::interiorFog(cell).mExtinction << " per unit\n";
    }
}

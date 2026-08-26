#pragma once

#include <string_view>

namespace ESM
{
    struct Cell;
}

namespace RtxTool
{
    class World;

    /// Prints where every object whose model path contains `needle` stands.
    ///
    /// A cell is thousands of references and a view wants to point at one of them. Grepping the
    /// content files gives a model name; this gives the place it was put.
    int runFind(World& world, const ESM::Cell& cell, std::string_view needle);
}

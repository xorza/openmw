#pragma once

#include <boost/program_options/options_description.hpp>

namespace RtxTool
{
    /// Every option the harness takes, on one description.
    ///
    /// **In the library rather than beside `main`, because standing a world up needs it.** A
    /// `World` is built from a `variables_map`, and a map is only usable once it has been notified
    /// against the description that declares its keys — so anything that reads a cell outside the
    /// tool, a test included, needs this and must not declare a second copy that can drift from it.
    ///
    /// @param validationByDefault what the three validation switches read when nobody names them.
    ///        Passed in rather than compiled in: the build turns the layers on outside a Release
    ///        build, and that is a decision about the *command line*, which only the executable
    ///        downstream of this has.
    boost::program_options::options_description makeOptionsDescription(bool validationByDefault);
}

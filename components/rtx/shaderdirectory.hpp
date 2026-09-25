#pragma once

#include <array>
#include <cstdint>
#include <filesystem>

namespace Rtx
{
    /// Where a renderer reads its compiled shaders under `resources`: the modules with their source
    /// taken out, which is what the driver's caches are keyed on, or the same modules with it, for a
    /// profiler that shows a shader's lines. `RTX_SPIRV_DIR` says why there are two.
    std::filesystem::path shaderDirectory(const std::filesystem::path& resources, bool withSource);

    /// A number that changes when any compiled shader in `directory` does, and with nothing else:
    /// every file's name and bytes, in name order. The whole directory, because a module none of
    /// one run's pipelines named may be named by the next. The modules without their source are
    /// under a megabyte and hash in a fifth of a millisecond once the system has read them, and the
    /// eight megabytes with it in one; nought where the directory cannot be read.
    std::array<std::uint64_t, 2> digestShaders(const std::filesystem::path& directory);
}

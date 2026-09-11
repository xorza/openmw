#pragma once

#include <optional>

#include <components/rtx/renderprofile.hpp>
#include <components/rtxbench/benchrun.hpp>

namespace MWRender
{
    /// What a harness run asks of the ray tracer, where a harness started this process. Carried
    /// through `RendererSpec`, so who owns the request and the result is readable off the
    /// signature; `GlRenderer` ignores it, which is why it hangs off the spec rather than sitting
    /// in it.
    struct RtxSetup
    {
        /// Nothing where the harness turned no knob, which is a run at whatever the settings say.
        std::optional<Rtx::RenderProfile> mProfile;

        /// Nothing where the harness asked for no measured run — `[RTX] session` is the other way
        /// in, and a played binary has only that one.
        std::optional<Rtx::SessionRequest> mSession;

        /// Where the run's answer goes. The caller's own, and it has to outlive `Engine::go`: the
        /// session fills it from its own destructor, which `~Engine` runs.
        Rtx::SessionResult* mInto = nullptr;
    };
}

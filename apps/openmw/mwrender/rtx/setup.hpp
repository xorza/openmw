#pragma once

#include <optional>

#include <components/rtx/renderprofile.hpp>
#include <components/rtxbench/benchrun.hpp>

namespace MWRender
{
    /// What a harness run asks of the ray tracer, where a harness started this process.
    ///
    /// **Carried through `RendererSpec` rather than through a global.** A file-static held the
    /// request and a raw pointer to the caller's result, written by `runHosted` and taken out by
    /// the renderer's constructor — a mailbox that made the ownership of a live object impossible
    /// to read off either side. The profile came across worse: it was written into the settings
    /// registry a field at a time and read back out of it.
    ///
    /// `GlRenderer` ignores this, which is why it hangs off the spec rather than sitting in it.
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

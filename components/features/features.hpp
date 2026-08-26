#ifndef OPENMW_COMPONENTS_FEATURES_FEATURES_H
#define OPENMW_COMPONENTS_FEATURES_FEATURES_H

/// What this binary was compiled with, as values rather than as macros.
///
/// **Not `Version`, which is what release this is.** A version identifies the source; this says
/// which optional parts of it the compiler was actually given.
namespace Features
{
    /// Whether the experimental ray tracing renderer was built in.
    ///
    /// **A value, so that nothing above has to ask the preprocessor.** `-DOPENMW_RTX=ON` is a fact
    /// about the binary, not a branch for everything that cares to repeat. Two places care: the
    /// launcher and the settings page both show the switch dead rather than hiding it, because a
    /// control that silently does nothing is worse than one that says why — and neither of them has
    /// any business naming a macro to find that out.
    ///
    /// **This is one of the two places the option is spelled, and the other cannot be helped:**
    /// `MWRender::createRenderer` has to name a type that does not exist in a build without it.
    /// Everything else asks here.
    bool hasRayTracing();
}

#endif

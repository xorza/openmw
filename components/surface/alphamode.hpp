#pragma once

namespace Surface
{
    /// What the alpha channel of a surface's diffuse texture means.
    ///
    /// `Cutout` and `Blend` are not exclusive in a NIF — `NiAlphaProperty` can ask for both — but no
    /// renderer benefits from honouring both, and the rasterizer already resolves them this way:
    /// blending wins, and the test threshold survives for a renderer that would rather cut out.
    ///
    /// **Its own header because a renderer's material names it and does not want the rest.** What
    /// the content said a surface is carries an `osg::Image` per texture role, and a renderer that
    /// only has to know what the alpha means should not pay for eleven of them.
    enum class AlphaMode
    {
        /// Ignore it. The overwhelming majority of Morrowind's geometry.
        Opaque,

        /// Test against the material's own threshold.
        Cutout,

        /// Blend. **This is where the foliage is**, and a ray tracer reads it the opposite of the
        /// obvious way: barely any of Morrowind's material set is alpha-tested outright — a canopy,
        /// a grate or a banner is an `NiAlphaProperty` over a texture whose alpha is all but binary,
        /// and the original renderer sorted it rather than testing it.
        /// `Rtx::Material::getAlphaCutoff` says what a renderer with no sort does about that.
        Blend,
    };
}

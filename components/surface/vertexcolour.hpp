#pragma once

namespace Surface
{
    /// What a surface's per-vertex colour is for, as the content said.
    ///
    /// **`NiVertexColorProperty`'s three vertex modes, resolved against its light mode.**
    /// `SceneUtil::VertexColorModes` carries six, because a loaded `osg::Material` can name the
    /// ambient, the diffuse or the specular alone — but a NIF states only these three. A renderer
    /// with one albedo folds the first two of those into the tint, and the specular has nowhere to
    /// go in a model with no specular lobe.
    ///
    /// **Its own header for the reason `AlphaMode` has one.** A renderer's material names this and
    /// does not want the eleven texture roles beside it.
    enum class VertexColour
    {
        /// The colours are there and mean nothing, or there are none at all. `NifOsg` resolves both
        /// to this, so a surface that says nothing here has nothing to apply.
        None,

        /// It replaces the material's diffuse and ambient colour, which is what
        /// `glColorMaterial(GL_AMBIENT_AND_DIFFUSE)` does and what the game's own shader reads
        /// through `getDiffuseColor`. Every piece of ground is this, and so is every model that
        /// carries colours and says nothing about them.
        Tint,

        /// It replaces the material's emissive colour. The light mode that goes with it also takes
        /// the diffuse and the ambient to nought, so such a surface is its glow and nothing else.
        Glow,
    };
}

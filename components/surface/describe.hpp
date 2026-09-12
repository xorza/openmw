#pragma once

namespace osg
{
    class StateSet;
}

namespace Surface
{
    struct Material;

    /// Folds what one state set says about a surface into `material`, and says whether it said
    /// anything at all.
    ///
    /// **Read off the finished state set, the way `Shader::ShaderVisitor` reads its own
    /// requirements.** Everything a description holds was written into OpenGL pipeline state by
    /// whoever loaded the content — a `NiMaterialProperty` became a `SceneUtil::Material`, a
    /// `NiAlphaProperty` an `osg::AlphaFunc` and a `BlendFunc`, a `NiStencilProperty` a
    /// `GL_CULL_FACE` mode, a texture a unit with a `SceneUtil::TextureType` beside it — so the
    /// state set is the one place the fact is kept, whatever loaded it and whatever a controller
    /// has done to it since. A description authored beside that state would be a second copy of
    /// the same fact, and the loader's thirty signatures would carry it.
    ///
    /// **One state set at a time, nearest last, because that is how OpenGL resolves a chain.** A
    /// texturing property three nodes up and a material on the shape land on two state sets, and
    /// the shape wears both; a caller folds the chain in force at a drawable in order and the
    /// later state set overrides what the earlier one set. A material starts as the defaults the
    /// loader would have written for a shape nothing spoke about.
    ///
    /// What is read, and from where:
    /// - a texture at a unit: its role is the `SceneUtil::TextureType` at that unit, or the sampler
    ///   uniform naming the unit — and a unit nothing names is not the surface's;
    /// - the colours, the opacity, the glossiness, the emissive multiplier and the vertex-colour
    ///   mode: the `SceneUtil::Material` attribute;
    /// - the opacity again from an `alpha` uniform, which is what `NifOsg::AlphaController`
    ///   animates — unless an `actorFade` stands beside it, in which case the pair is the game
    ///   fading an actor and `Rtx::fadeThrough`'s business;
    /// - the alpha test: the `osg::AlphaFunc` attribute, whose reference the visitor moves into an
    ///   `alphaRef` uniform when it replaces the attribute with `Shader::RemovedAlphaFunc`;
    /// - blending: a `BlendFunc` attribute or the `GL_BLEND` mode;
    /// - two-sidedness: the `GL_CULL_FACE` mode, which only a stencil property or a material file
    ///   turns off;
    /// - the texture transform: the `texMat<unit>` uniform on the diffuse unit, undone to the scale
    ///   and offset it was built from.
    ///
    /// @return whether the state set carried a material or a texture: what tells a surface from a
    ///         node that only sets a mode or a uniform on the way down.
    bool describe(const osg::StateSet& stateSet, Material& material);
}

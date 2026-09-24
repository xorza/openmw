#pragma once

#include <array>
#include <string>
#include <string_view>

#include <osg/NodeVisitor>
#include <osg/StateSet>

namespace osg
{
    class Texture;
}

namespace Resource
{
    class ImageManager;
}

namespace Shader
{
    /// Every texture type a shader reads by name. `normalHeightMap` is not among them: a shader
    /// reads it as `normalMap`.
    inline constexpr std::array<std::string_view, 10> sDefaultTextures = { "diffuseMap", "normalMap", "emissiveMap",
        "darkMap", "detailMap", "envMap", "specularMap", "decalMap", "bumpMap", "glossMap" };

    /// The name a shader reads the texture at `unit` by: its `SceneUtil::TextureType`, or
    /// `diffuseMap` at unit nought where that is neither in `sDefaultTextures` nor
    /// `normalHeightMap`. Good for as long as the state set is.
    std::string_view textureNameAt(const osg::StateSet& stateSet, const osg::Texture& texture, unsigned int unit);

    /// `node`'s state set to change: the node's own where it has none yet, and a shallow copy set on
    /// the node where it has one, because that one may be shared or already drawn.
    osg::StateSet* getWritableStateSet(osg::Node& node);

    /// The `[Shaders]` switches and patterns that lead from a diffuse texture's file name to its
    /// companion maps: `foo.dds` to `foo_nh.dds`, or to `foo_n.dds` where there is no `_nh`, and to
    /// `foo_spec.dds`.
    struct AutoMapRules
    {
        bool mNormalMaps = false;
        std::string mNormalMapPattern;
        std::string mNormalHeightMapPattern;

        bool mSpecularMaps = false;
        std::string mSpecularMapPattern;

        bool any() const { return mNormalMaps || mSpecularMaps; }
    };

    /// What `attachAutoMaps` added, with the unit of each map, or -1 where it added none.
    struct AttachedMaps
    {
        const osg::Texture* mNormalMap = nullptr;
        int mNormalUnit = -1;

        /// Whether the normal map is the `_nh` file, which carries height in its alpha.
        bool mNormalHeight = false;

        int mSpecularUnit = -1;
    };

    /// Adds the maps `diffuseMap`'s file name leads to under `rules`, where the state set has none of
    /// that kind: each at the next free unit of `units`, with its `SceneUtil::TextureType`, and
    /// addressed and filtered as the diffuse map is.
    ///
    /// @param normalMap,specularMap what the state set already binds, which an added map would
    ///        replace, so none is added in their place.
    /// @param bumpMap what the state set binds as a bump map. A normal map with its file name is the
    ///        same file, and is not added.
    /// @param writable the state set to add to, or null to take `getWritableStateSet(node)` at the
    ///        first map found.
    AttachedMaps attachAutoMaps(const AutoMapRules& rules, Resource::ImageManager& images,
        const osg::Texture& diffuseMap, const osg::Texture* normalMap, const osg::Texture* specularMap,
        const osg::Texture* bumpMap, const osg::StateSet::TextureAttributeList& units, osg::StateSet*& writable,
        osg::Node& node);

    /// Attaches the companion maps and changes nothing else, for a renderer that runs no
    /// `ShaderVisitor`. It visits the state sets the shader visitor visits and applies the same rule,
    /// so both renderers find the same maps.
    class AutoMapVisitor : public osg::NodeVisitor
    {
    public:
        /// @param rules held by reference, for the one traversal this is made for.
        AutoMapVisitor(const AutoMapRules& rules, Resource::ImageManager& images);

        void apply(osg::Node& node) override;
        void apply(osg::Drawable& drawable) override;

    private:
        void attach(osg::Node& node);

        const AutoMapRules& mRules;
        Resource::ImageManager& mImages;
    };
}

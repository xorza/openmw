#pragma once

#include <array>
#include <functional>
#include <string>
#include <string_view>

#include <osg/NodeVisitor>
#include <osg/StateSet>

namespace osg
{
    class Drawable;
    class Geometry;
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

    /// Runs `adjust` on the geometry a skinned or morphed `drawable` is drawn from, and sets that
    /// geometry again where `adjust` answers that it changed it: the copies the drawable draws from
    /// are made when it is set. False where `drawable` is neither, and `adjust` did not run.
    bool adjustSourceGeometry(osg::Drawable& drawable, const std::function<bool(osg::Geometry&)>& adjust);

    /// The texture unit whose coordinate array holds a geometry's tangents, which is where
    /// `ShaderVisitor` puts them and where `SceneUtil::RigGeometry` poses them from.
    inline constexpr unsigned int sTangentUnit = 7;

    /// What a renderer that runs no `ShaderVisitor` still needs of it, and nothing else: the
    /// companion maps, and the tangents a normal map is read through. It visits the state sets the
    /// shader visitor visits and applies the same rules, so both renderers find the same maps and
    /// read them through the same tangents.
    class MapVisitor : public osg::NodeVisitor
    {
    public:
        /// @param rules held by reference, for the one traversal this is made for.
        MapVisitor(const AutoMapRules& rules, Resource::ImageManager& images);

        void apply(osg::Node& node) override;
        void apply(osg::Drawable& drawable) override;

    private:
        /// Attaches what `node`'s own state set leads to, and notes its normal map where it has
        /// one, bound or attached.
        void attach(osg::Node& node);

        /// Builds `geometry`'s tangents at `sTangentUnit` with `osgUtil::TangentSpaceGenerator`,
        /// from the coordinates the normal map in force reads, as `ShaderVisitor::adjustGeometry`
        /// does. Answers whether it built any.
        bool buildTangents(osg::Geometry& geometry) const;

        const AutoMapRules& mRules;
        Resource::ImageManager& mImages;

        /// The unit of the normal map in force at the node being visited, or -1 where there is
        /// none: set by the nearest state set that binds one and carried down to what it shades, as
        /// the shader visitor carries its requirements.
        int mNormalUnit = -1;
    };
}

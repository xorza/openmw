#include "material.hpp"

#include <osg/Texture>

namespace Surface
{
    namespace
    {
        constexpr std::array<std::string_view, sTextureRoleCount> sRoleNames = {
            "diffuseMap",
            "normalMap",
            "normalHeightMap",
            "emissiveMap",
            "specularMap",
            "darkMap",
            "detailMap",
            "decalMap",
            "glossMap",
            "bumpMap",
            "envMap",
        };
    }

    void Material::setTexture(TextureRole role, const osg::Texture* texture)
    {
        mTextures[static_cast<std::size_t>(role)] = texture != nullptr ? texture->getImage(0) : nullptr;
    }

    std::string_view textureRoleName(TextureRole role)
    {
        return sRoleNames[static_cast<std::size_t>(role)];
    }

    std::optional<TextureRole> textureRoleNamed(std::string_view name)
    {
        for (std::size_t i = 0; i < sRoleNames.size(); ++i)
            if (sRoleNames[i] == name)
                return static_cast<TextureRole>(i);

        return std::nullopt;
    }
}

#include "automaps.hpp"

#include <algorithm>
#include <string>

#include <osg/Drawable>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Node>
#include <osg/Texture2D>
#include <osgUtil/TangentSpaceGenerator>

#include <components/misc/strings/algorithm.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/sceneutil/util.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

namespace Shader
{
    namespace
    {
        /// The texture a companion map is bound as: the diffuse map's addressing and filtering over
        /// the companion's image.
        osg::ref_ptr<osg::Texture2D> companionTexture(osg::Image& image, const osg::Texture& diffuseMap)
        {
            osg::ref_ptr<osg::Texture2D> texture(new osg::Texture2D(&image));
            texture->setTextureSize(image.s(), image.t());
            texture->setWrap(osg::Texture::WRAP_S, diffuseMap.getWrap(osg::Texture::WRAP_S));
            texture->setWrap(osg::Texture::WRAP_T, diffuseMap.getWrap(osg::Texture::WRAP_T));
            texture->setFilter(osg::Texture::MIN_FILTER, diffuseMap.getFilter(osg::Texture::MIN_FILTER));
            texture->setFilter(osg::Texture::MAG_FILTER, diffuseMap.getFilter(osg::Texture::MAG_FILTER));
            texture->setMaxAnisotropy(diffuseMap.getMaxAnisotropy());
            return texture;
        }

        bool isTextureNameRecognized(std::string_view name)
        {
            return std::ranges::find(sDefaultTextures, name) != sDefaultTextures.end() || name == "normalHeightMap";
        }

        /// `fileName` with `pattern` before its extension, as a path the VFS can be asked about.
        VFS::Path::Normalized companionPath(std::string fileName, const std::string& pattern)
        {
            Misc::StringUtils::replaceLast(fileName, ".", pattern + ".");
            return VFS::Path::Normalized(fileName);
        }
    }

    std::string_view textureNameAt(const osg::StateSet& stateSet, const osg::Texture& texture, unsigned int unit)
    {
        const std::string_view name = SceneUtil::getTextureType(stateSet, texture, unit);
        if ((name.empty() || !isTextureNameRecognized(name)) && unit == 0)
            return "diffuseMap";
        return name;
    }

    osg::StateSet* getWritableStateSet(osg::Node& node)
    {
        if (!node.getStateSet())
            return node.getOrCreateStateSet();

        osg::ref_ptr<osg::StateSet> newStateSet = new osg::StateSet(*node.getStateSet(), osg::CopyOp::SHALLOW_COPY);
        node.setStateSet(newStateSet);
        return newStateSet.get();
    }

    AttachedMaps attachAutoMaps(const AutoMapRules& rules, Resource::ImageManager& images,
        const osg::Texture& diffuseMap, const osg::Texture* normalMap, const osg::Texture* specularMap,
        const osg::Texture* bumpMap, const osg::StateSet::TextureAttributeList& units, osg::StateSet*& writable,
        osg::Node& node)
    {
        AttachedMaps attached;

        const osg::Image* const diffuseImage = diffuseMap.getImage(0);
        if (diffuseImage == nullptr)
            return attached;

        const VFS::Manager& vfs = *images.getVFS();

        if (rules.mNormalMaps && normalMap == nullptr)
        {
            osg::ref_ptr<osg::Image> image;
            bool normalHeight = false;
            const VFS::Path::Normalized normalHeightPath
                = companionPath(diffuseImage->getFileName(), rules.mNormalHeightMapPattern);
            if (vfs.exists(normalHeightPath))
            {
                image = images.getImage(normalHeightPath);
                normalHeight = true;
            }
            else
            {
                const VFS::Path::Normalized normalPath
                    = companionPath(diffuseImage->getFileName(), rules.mNormalMapPattern);
                if (vfs.exists(normalPath))
                    image = images.getImage(normalPath);
            }

            // A normal map already bound as the bump map is the same file, and probably not a
            // normal map at all.
            const bool hasNamesakeBumpMap = image && bumpMap && bumpMap->getImage(0)
                && image->getFileName() == bumpMap->getImage(0)->getFileName();

            if (!hasNamesakeBumpMap && image)
            {
                osg::ref_ptr<osg::Texture2D> texture = companionTexture(*image, diffuseMap);

                const int unit = static_cast<int>(units.size());
                if (!writable)
                    writable = getWritableStateSet(node);
                writable->setTextureAttribute(unit, texture, osg::StateAttribute::ON);
                writable->setTextureAttribute(unit,
                    new SceneUtil::TextureType(normalHeight ? "normalHeightMap" : "normalMap"),
                    osg::StateAttribute::ON);

                attached.mNormalMap = texture.get();
                attached.mNormalUnit = unit;
                attached.mNormalHeight = normalHeight;
            }
        }

        if (rules.mSpecularMaps && specularMap == nullptr)
        {
            const VFS::Path::Normalized specularPath
                = companionPath(diffuseImage->getFileName(), rules.mSpecularMapPattern);
            if (vfs.exists(specularPath))
            {
                osg::ref_ptr<osg::Image> image(images.getImage(specularPath));
                osg::ref_ptr<osg::Texture2D> texture = companionTexture(*image, diffuseMap);

                // After the normal map, which the units counted where the state set added to is the
                // one they list.
                const int unit = static_cast<int>(units.size());
                if (!writable)
                    writable = getWritableStateSet(node);
                writable->setTextureAttribute(unit, texture, osg::StateAttribute::ON);
                writable->setTextureAttribute(unit, new SceneUtil::TextureType("specularMap"), osg::StateAttribute::ON);

                attached.mSpecularUnit = unit;
            }
        }

        return attached;
    }

    bool adjustSourceGeometry(osg::Drawable& drawable, const std::function<bool(osg::Geometry&)>& adjust)
    {
        if (auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
        {
            osg::ref_ptr<osg::Geometry> source = rig->getSourceGeometry();
            if (source != nullptr && adjust(*source))
                rig->setSourceGeometry(std::move(source));
            return true;
        }

        if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
        {
            osg::ref_ptr<osg::Geometry> source = morph->getSourceGeometry();
            if (source != nullptr && adjust(*source))
                morph->setSourceGeometry(std::move(source));
            return true;
        }

        if (auto* holder = dynamic_cast<SceneUtil::RigGeometryHolder*>(&drawable))
        {
            osg::ref_ptr<SceneUtil::OsgaRigGeometry> rigged = holder->getSourceRigGeometry();
            osg::ref_ptr<osg::Geometry> source = rigged->getSourceGeometry();
            if (source != nullptr && adjust(*source))
            {
                rigged->setSourceGeometry(std::move(source));
                holder->setSourceRigGeometry(std::move(rigged));
            }
            return true;
        }

        return false;
    }

    MapVisitor::MapVisitor(const AutoMapRules& rules, Resource::ImageManager& images)
        : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        , mRules(rules)
        , mImages(images)
    {
    }

    void MapVisitor::apply(osg::Node& node)
    {
        const int above = mNormalUnit;
        attach(node);
        traverse(node);
        mNormalUnit = above;
    }

    void MapVisitor::apply(osg::Drawable& drawable)
    {
        const int above = mNormalUnit;
        attach(drawable);

        // A skinned or morphed drawable's tangents belong on its source geometry: `RigGeometry`
        // poses them from there.
        if (mNormalUnit >= 0
            && !adjustSourceGeometry(drawable, [this](osg::Geometry& source) { return buildTangents(source); }))
        {
            if (osg::Geometry* geometry = drawable.asGeometry())
                buildTangents(*geometry);
        }

        mNormalUnit = above;
    }

    void MapVisitor::attach(osg::Node& node)
    {
        osg::StateSet* const stateSet = node.getStateSet();
        if (stateSet == nullptr)
            return;

        const osg::Texture* diffuseMap = nullptr;
        const osg::Texture* normalMap = nullptr;
        const osg::Texture* specularMap = nullptr;
        const osg::Texture* bumpMap = nullptr;
        int normalUnit = -1;

        const osg::StateSet::TextureAttributeList& units = stateSet->getTextureAttributeList();
        for (unsigned int unit = 0; unit < units.size(); ++unit)
        {
            const osg::StateAttribute* attribute = stateSet->getTextureAttribute(unit, osg::StateAttribute::TEXTURE);
            const osg::Texture* texture = attribute != nullptr ? attribute->asTexture() : nullptr;
            if (texture == nullptr)
                continue;

            const std::string_view name = textureNameAt(*stateSet, *texture, unit);
            if (name == "diffuseMap")
                diffuseMap = texture;
            else if (name == "normalMap" || name == "normalHeightMap")
            {
                normalMap = texture;
                normalUnit = static_cast<int>(unit);
            }
            else if (name == "specularMap")
                specularMap = texture;
            else if (name == "bumpMap")
                bumpMap = texture;
        }

        if (diffuseMap != nullptr)
        {
            // Loading is the one time the state set is only the loader's, so it is added to in place.
            osg::StateSet* writable = stateSet;
            const AttachedMaps attached
                = attachAutoMaps(mRules, mImages, *diffuseMap, normalMap, specularMap, bumpMap, units, writable, node);
            if (attached.mNormalMap != nullptr)
                normalUnit = attached.mNormalUnit;
        }

        if (normalUnit >= 0)
            mNormalUnit = normalUnit;
    }

    bool MapVisitor::buildTangents(osg::Geometry& geometry) const
    {
        // The coordinates the normal map is read through: its own unit's, or unit nought's where it
        // has none, or the first array there is where unit nought has none either — which is what
        // `ShaderVisitor::adjustGeometry` binds at the normal map's unit before it builds from it.
        unsigned int unit = static_cast<unsigned int>(mNormalUnit);
        if (geometry.getTexCoordArray(unit) == nullptr)
            unit = 0;
        if (geometry.getTexCoordArray(unit) == nullptr)
        {
            const osg::Geometry::ArrayList& arrays = geometry.getTexCoordArrayList();
            unsigned int found = 0;
            while (found < arrays.size() && (found == sTangentUnit || arrays[found] == nullptr))
                ++found;
            if (found == arrays.size())
                return false;
            unit = found;
        }

        osg::ref_ptr<osgUtil::TangentSpaceGenerator> generator(new osgUtil::TangentSpaceGenerator);
        generator->generate(&geometry, static_cast<int>(unit));
        geometry.setTexCoordArray(sTangentUnit, generator->getTangentArray(), osg::Array::BIND_PER_VERTEX);
        return true;
    }
}

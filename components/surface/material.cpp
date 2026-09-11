#include "material.hpp"

#include <osg/StateSet>
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

        /// The material as something `osg::UserDataContainer` will hold.
        class Holder : public osg::Object
        {
        public:
            Holder() = default;

            explicit Holder(const Material& material)
                : mMaterial(material)
            {
            }

            Holder(const Holder& other, const osg::CopyOp& copyOp)
                : osg::Object(other, copyOp)
                , mMaterial(other.mMaterial)
            {
            }

            META_Object(Surface, Holder)

            Material mMaterial;
        };

        const Holder sPrototype;

        /// `META_Object` answers both names with one string literal per class, so a holder is told
        /// from anything else in the container by two pointer compares. `setMaterial` puts it at
        /// slot nought, so nothing after it is looked at: the lookup runs per state set in force,
        /// per drawable, per frame.
        const Holder* holderIn(const osg::UserDataContainer& container)
        {
            if (container.getNumUserObjects() == 0)
                return nullptr;

            const osg::Object* first = container.getUserObject(0);
            if (first == nullptr || first->libraryName() != sPrototype.libraryName()
                || first->className() != sPrototype.className())
                return nullptr;

            return static_cast<const Holder*>(first);
        }
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

    namespace
    {
        /// Written once by the host before it loads anything, and read from many threads after that.
        /// No synchronisation, because there is no write to race with by the time content arrives.
        bool sDescribing = false;
    }

    void describeSurfaces(bool describe)
    {
        sDescribing = describe;
    }

    void setMaterial(osg::StateSet& stateSet, const Material& material)
    {
        if (!sDescribing)
            return;

        if (Material* writable = getWritableMaterial(stateSet))
        {
            *writable = material;
            return;
        }

        // At slot nought, which `holderIn` reads and nothing else. Whatever stood there moves to
        // the end: the container has no insert, and nothing reads a state set's user objects by
        // position but this.
        osg::UserDataContainer& container = *stateSet.getOrCreateUserDataContainer();
        if (container.getNumUserObjects() == 0)
        {
            container.addUserObject(new Holder(material));
            return;
        }

        const osg::ref_ptr<osg::Object> displaced = container.getUserObject(0);
        container.setUserObject(0, new Holder(material));
        container.addUserObject(displaced);
    }

    const Material* getMaterial(const osg::StateSet& stateSet)
    {
        const osg::UserDataContainer* container = sDescribing ? stateSet.getUserDataContainer() : nullptr;
        if (container == nullptr)
            return nullptr;

        const Holder* holder = holderIn(*container);
        return holder != nullptr ? &holder->mMaterial : nullptr;
    }

    Material* getWritableMaterial(osg::StateSet& stateSet)
    {
        // The rasterizer runs the same controllers and has nothing to write into: what it would
        // find here is what `setMaterial` never made.
        if (!sDescribing)
            return nullptr;

        osg::UserDataContainer* container = stateSet.getUserDataContainer();
        if (container == nullptr || holderIn(*container) == nullptr)
            return nullptr;

        // A shallow-copied state set shares the container, and the container shares the holder;
        // each is duplicated where something else can still see it, so the write reaches this
        // state set and no other.
        if (container->referenceCount() > 1)
        {
            osg::ref_ptr<osg::UserDataContainer> mine
                = static_cast<osg::UserDataContainer*>(container->clone(osg::CopyOp::SHALLOW_COPY));
            stateSet.setUserDataContainer(mine);
            container = mine;
        }

        Holder* holder = static_cast<Holder*>(container->getUserObject(0));
        if (holder->referenceCount() > 1)
        {
            osg::ref_ptr<Holder> mine = new Holder(holder->mMaterial);
            container->setUserObject(0, mine);
            holder = mine;
        }

        return &holder->mMaterial;
    }
}

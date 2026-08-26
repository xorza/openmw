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

        /// The user-data slot the material lives in. A name rather than the single `setUserData`
        /// pointer, which belongs to whoever else wants it.
        constexpr std::string_view sUserObjectName = "SurfaceMaterial";

        /// No such object in this container.
        constexpr unsigned int sNowhere = ~0u;

        /// Where the material sits in `container`, or `sNowhere`.
        ///
        /// **Scanned here rather than asked for by name, and not because it is faster.**
        /// `osg::UserDataContainer::getUserObject` takes a `std::string`, so looking a material up
        /// by name builds one — once per state set in force, per drawable, per frame, which on an
        /// exterior is tens of thousands of times. Measured, that costs nothing: the walk is 2.0 ms
        /// a frame either way, because the name is fifteen characters and a `std::string` holds
        /// fifteen without reaching for the allocator.
        ///
        /// **Fifteen is exactly the limit**, so the name is one character from putting an
        /// allocation on the frame path with nothing to say it had. The loop below is the one that
        /// call would make anyway, over a container that almost always holds this and nothing
        /// else, and it cannot be pushed over that edge by renaming a string.
        unsigned int findMaterial(const osg::UserDataContainer& container)
        {
            const unsigned int count = container.getNumUserObjects();
            for (unsigned int at = 0; at < count; ++at)
            {
                const osg::Object* object = container.getUserObject(at);
                if (object != nullptr && object->getName() == sUserObjectName)
                    return at;
            }

            return sNowhere;
        }

        /// The material as something `osg::UserDataContainer` will hold.
        class Holder : public osg::Object
        {
        public:
            /// The name is what `getMaterial` searches the container by, so it is set here rather
            /// than in the constructor that takes a material: `META_Object` needs a default one too,
            /// and a nameless holder is one nothing can find again.
            Holder() { setName(std::string(sUserObjectName)); }

            explicit Holder(const Material& material)
                : Holder()
            {
                mMaterial = material;
            }

            Holder(const Holder& other, const osg::CopyOp& copyOp)
                : osg::Object(other, copyOp)
                , mMaterial(other.mMaterial)
            {
            }

            META_Object(Surface, Holder)

            Material mMaterial;
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

    void setMaterial(osg::StateSet& stateSet, const Material& material)
    {
        if (Material* writable = getWritableMaterial(stateSet))
        {
            *writable = material;
            return;
        }

        stateSet.getOrCreateUserDataContainer()->addUserObject(new Holder(material));
    }

    const Material* getMaterial(const osg::StateSet& stateSet)
    {
        const osg::UserDataContainer* container = stateSet.getUserDataContainer();
        if (container == nullptr)
            return nullptr;

        const unsigned int at = findMaterial(*container);
        if (at == sNowhere)
            return nullptr;

        return &static_cast<const Holder*>(container->getUserObject(at))->mMaterial;
    }

    Material* getWritableMaterial(osg::StateSet& stateSet)
    {
        osg::UserDataContainer* container = stateSet.getUserDataContainer();
        if (container == nullptr)
            return nullptr;

        // **A shallow copy shares the container itself, not a copy of it.** `osg::Object`'s copy
        // constructor takes the pointer, so a `SceneUtil::StateSetUpdater`'s scratch — which is a
        // shallow copy of the node's state set — writes straight into the node's own user data
        // unless it is given a container of its own first. Shallow, because the objects in it are
        // read-only values every copy is happy to share.
        if (container->referenceCount() > 1)
        {
            osg::ref_ptr<osg::UserDataContainer> mine
                = static_cast<osg::UserDataContainer*>(container->clone(osg::CopyOp::SHALLOW_COPY));
            stateSet.setUserDataContainer(mine);
            container = mine;
        }

        const unsigned int at = findMaterial(*container);
        if (at == sNowhere)
            return nullptr;

        Holder* holder = static_cast<Holder*>(container->getUserObject(at));

        // And the holder outlives the split: two containers now point at the one description, so it
        // is duplicated before it is written to. Once, because the copy is then the only holder.
        if (holder->referenceCount() > 1)
        {
            osg::ref_ptr<Holder> mine = new Holder(holder->mMaterial);
            container->setUserObject(at, mine);
            holder = mine;
        }

        return &holder->mMaterial;
    }
}

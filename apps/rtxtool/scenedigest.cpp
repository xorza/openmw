#include "scenedigest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <components/rtx/scenedesc.hpp>

#include <components/rtxbench/framehashes.hpp>

namespace RtxTool
{
    namespace
    {
        /// A digest that no order of its parts can tell: each part's words are added into the whole.
        class Unordered
        {
        public:
            void add(const Rtx::Digest& part)
            {
                mWords[0] += part.getWords()[0];
                mWords[1] += part.getWords()[1];
            }

            const std::array<std::uint64_t, 2>& getWords() const { return mWords; }

        private:
            std::array<std::uint64_t, 2> mWords{};
        };

        void addTexture(Rtx::Digest& digest, const Rtx::SceneDesc& scene, const Rtx::Index texture)
        {
            digest.add(texture == Rtx::sNoIndex);
            if (texture == Rtx::sNoIndex)
                return;

            const std::string_view path = scene.getTextures()[texture].value();
            digest.add(std::span<const char>(path.data(), path.size()));
        }

        void addMaterial(Rtx::Digest& digest, const Rtx::SceneDesc& scene, const Rtx::Index index)
        {
            digest.add(index == Rtx::sNoIndex);
            if (index == Rtx::sNoIndex)
                return;

            const Rtx::Material& material = scene.getMaterials()[index];
            digest.add(material.mKind);
            addTexture(digest, scene, material.mDiffuse);
            addTexture(digest, scene, material.mNormal);
            addTexture(digest, scene, material.mEmissive);
            digest.add(material.mDiffuseColour);
            digest.add(material.mEmissiveColour);
            digest.add(material.mAlphaRef);
            digest.add(material.mAlphaMode);
            digest.add(material.mTwoSided);
            digest.add(material.mTextureTransform);
            digest.add(material.mLayerCount);

            for (Rtx::Index at = 0; at < material.mLayerCount; ++at)
            {
                const Rtx::MaterialLayer& layer = scene.getLayers()[material.mLayerOffset + at];
                addTexture(digest, scene, layer.mDiffuse);
                digest.add(layer.mDiffuseTransform);
                digest.add(layer.mMaskTransform);
                digest.add(scene.getMasks().subspan(
                    layer.mMaskOffset, static_cast<std::size_t>(layer.mMaskWidth) * layer.mMaskHeight));
            }
        }

        /// One corner of a triangle, as the picture sees it.
        struct Corner
        {
            osg::Vec3f mPosition;
            osg::Vec3f mNormal;
            osg::Vec2f mTexCoord;

            bool operator<(const Corner& other) const
            {
                return std::tie(mPosition, mNormal, mTexCoord)
                    < std::tie(other.mPosition, other.mNormal, other.mTexCoord);
            }
        };

        void addCorner(Rtx::Digest& digest, const Corner& corner)
        {
            digest.add(corner.mPosition);
            digest.add(corner.mNormal);
            digest.add(corner.mTexCoord);
        }

        /// A shape as the multiset of its triangles, each turned to start at its least corner so
        /// that the winding survives and the corner it happens to be spelt from does not.
        Unordered digestTriangles(const Rtx::SceneDesc& scene, const Rtx::MeshRange& mesh)
        {
            Unordered triangles;
            const std::span<const std::uint32_t> indices
                = scene.getIndices().subspan(mesh.mIndexOffset, mesh.mIndexCount);
            for (std::size_t at = 0; at + 2 < indices.size(); at += 3)
            {
                std::array<Corner, 3> corners;
                for (std::size_t corner = 0; corner < 3; ++corner)
                {
                    const std::size_t vertex = mesh.mVertexOffset + indices[at + corner];
                    corners[corner] = Corner{ scene.getPositions()[vertex], scene.getNormals()[vertex],
                        scene.getTexCoords()[vertex] };
                }

                const std::size_t least
                    = static_cast<std::size_t>(std::min_element(corners.begin(), corners.end()) - corners.begin());

                Rtx::Digest triangle;
                for (std::size_t corner = 0; corner < 3; ++corner)
                    addCorner(triangle, corners[(least + corner) % 3]);
                triangles.add(triangle);
            }

            return triangles;
        }

        void addMesh(Rtx::Digest& digest, const Rtx::SceneDesc& scene, const Rtx::Index index)
        {
            const Rtx::MeshRange& mesh = scene.getMeshes()[index];
            digest.add(digestTriangles(scene, mesh).getWords());
            digest.add(mesh.mDeform);
        }
    }

    std::string digestScene(const Rtx::SceneDesc& scene)
    {
        Unordered whole;

        for (const Rtx::MeshInstance& instance : scene.getInstances())
        {
            if (instance.mMesh == Rtx::sNoIndex)
                continue;

            Rtx::Digest placement;
            placement.add(std::span<const float>(instance.mTransform.ptr(), 16));
            placement.add(instance.mOpacity);
            placement.add(instance.mFirstPerson);
            addMaterial(placement, scene, instance.mMaterial);
            addMesh(placement, scene, instance.mMesh);
            whole.add(placement);
        }

        for (const Rtx::Light& light : scene.getLights())
        {
            Rtx::Digest lamp;
            lamp.add(light.mPosition);
            lamp.add(light.mIntensity);
            lamp.add(light.mReach);
            whole.add(lamp);
        }

        for (const Rtx::SpriteEmitter& emitter : scene.getEmitters())
        {
            Rtx::Digest plume;
            plume.add(emitter.mCentre);
            plume.add(emitter.mReach);
            plume.add(emitter.mAdditive);
            addTexture(plume, scene, emitter.mTexture);
            for (const Rtx::Sprite& sprite : scene.getSprites().subspan(emitter.mFirst, emitter.mCount))
            {
                plume.add(sprite.mPosition);
                plume.add(sprite.mRadius);
                plume.add(sprite.mColour);
                plume.add(sprite.mAlpha);
            }
            whole.add(plume);
        }
        return Rtx::spellHash(whole.getWords());
    }
}

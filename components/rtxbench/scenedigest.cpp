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
#include <components/vfs/pathutil.hpp>

#include "framehashes.hpp"

namespace Rtx
{
    namespace
    {
        /// A digest that no order of its parts can tell: each part's words are added into the whole.
        class Unordered
        {
        public:
            void add(const Digest& part)
            {
                mWords[0] += part.getWords()[0];
                mWords[1] += part.getWords()[1];
            }

            const std::array<std::uint64_t, 2>& getWords() const { return mWords; }

        private:
            std::array<std::uint64_t, 2> mWords{};
        };

        void addTexture(Digest& digest, const SceneDesc& scene, const Index texture)
        {
            digest.add(texture == sNoIndex);
            if (texture == sNoIndex)
                return;

            const std::string_view path = scene.getTextures()[texture].value();
            digest.add(std::span<const char>(path.data(), path.size()));
        }

        void addMaterial(Digest& digest, const SceneDesc& scene, const Index index)
        {
            digest.add(index == sNoIndex);
            if (index == sNoIndex)
                return;

            const Material& material = scene.getMaterials()[index];
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

            for (Index at = 0; at < material.mLayerCount; ++at)
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

        void addCorner(Digest& digest, const Corner& corner)
        {
            digest.add(corner.mPosition);
            digest.add(corner.mNormal);
            digest.add(corner.mTexCoord);
        }

        /// A shape as the multiset of its triangles, each turned to start at its least corner so
        /// that the winding survives and the corner it happens to be spelt from does not.
        Unordered digestTriangles(const SceneDesc& scene, const MeshRange& mesh)
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

                Digest triangle;
                for (std::size_t corner = 0; corner < 3; ++corner)
                    addCorner(triangle, corners[(least + corner) % 3]);
                triangles.add(triangle);
            }

            return triangles;
        }

        void addMesh(Digest& digest, const SceneDesc& scene, const Index index)
        {
            const MeshRange& mesh = scene.getMeshes()[index];
            digest.add(digestTriangles(scene, mesh).getWords());
            digest.add(mesh.mDeform);
        }
    }

    std::array<std::uint64_t, 2> digestScene(const SceneDesc& scene)
    {
        Unordered whole;

        for (const Rtx::MeshInstance& instance : scene.getInstances())
        {
            if (instance.mMesh == sNoIndex)
                continue;

            Digest placement;
            placement.add(std::span<const float>(instance.mTransform.ptr(), 16));
            placement.add(instance.mOpacity);
            placement.add(instance.mFirstPerson);
            addMaterial(placement, scene, instance.mMaterial);
            addMesh(placement, scene, instance.mMesh);
            whole.add(placement);
        }

        for (const Rtx::Light& light : scene.getLights())
        {
            Digest lamp;
            lamp.add(light.mPosition);
            lamp.add(light.mIntensity);
            lamp.add(light.mReach);
            whole.add(lamp);
        }

        for (const Rtx::SpriteEmitter& emitter : scene.getEmitters())
        {
            Digest plume;
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
        return whole.getWords();
    }

    /// The tables `digestLayout` reads whole, held to having nothing between their fields.
    ///
    /// **A field added later that opens a gap trips this rather than the digest.** The bytes a
    /// record pads with are whatever the allocator left, so a table read whole through one of them
    /// would call two identical runs different — once, unrepeatably, and for a reason nothing in
    /// the report could name.
    static_assert(sizeof(Light) == 36, "Light is read whole and must have no padding");
    static_assert(sizeof(Sprite) == 56, "Sprite is read whole and must have no padding");
    static_assert(sizeof(MaterialLayer) == 44, "MaterialLayer is read whole and must have no padding");
    static_assert(sizeof(Rig) == 24, "Rig is read whole and must have no padding");
    static_assert(sizeof(Morph) == 16, "Morph is read whole and must have no padding");

    std::array<std::uint64_t, 2> digestLayout(const SceneDesc& scene)
    {
        Digest whole;

        // The shared buffers, which is where a merge that ran in heap order shows and the largest
        // part of what this costs.
        whole.add(scene.getPositions());
        whole.add(scene.getNormals());
        whole.add(scene.getTexCoords());
        whole.add(scene.getIndices());

        // **Every slot, standing or free.** A free one keeps the room and the offsets its last
        // occupant left, so it is part of the state a run has to repeat — and a slot order that
        // moved is exactly what `digestScene` sums away.
        for (const MeshRange& mesh : scene.getMeshes())
        {
            whole.add(mesh.mVertexOffset);
            whole.add(mesh.mVertexCount);
            whole.add(mesh.mIndexOffset);
            whole.add(mesh.mIndexCount);
            whole.add(mesh.mShape.mSheet);
            whole.add(mesh.mShape.mClosed);
            whole.add(mesh.mDeform);
            whole.add(mesh.mDeformer);
            whole.add(mesh.mMaterial);
            whole.add(mesh.mBindOffset);
            whole.add(mesh.mPoseOffset);
            whole.add(mesh.mPosed);
            whole.add(mesh.mBounds._min);
            whole.add(mesh.mBounds._max);
        }

        for (const MeshInstance& instance : scene.getInstances())
        {
            whole.add(std::span<const float>(instance.mTransform.ptr(), 16));
            whole.add(instance.mMesh);
            whole.add(instance.mMaterial);
            whole.add(instance.mOpacity);
            whole.add(instance.mFirstPerson);
        }

        // Where each slot stood last frame, which is what a motion vector is the difference of.
        whole.add(scene.getPrevious());

        for (const Material& material : scene.getMaterials())
        {
            whole.add(material.mKind);
            whole.add(material.mDiffuse);
            whole.add(material.mNormal);
            whole.add(material.mEmissive);
            whole.add(material.mDiffuseColour);
            whole.add(material.mEmissiveColour);
            whole.add(material.mAlphaRef);
            whole.add(material.mAlphaMode);
            whole.add(material.mTwoSided);
            whole.add(material.mTextureTransform);
            whole.add(material.mLayerOffset);
            whole.add(material.mLayerCount);
            whole.add(material.mFlatten);
            whole.add(material.mAnimated);
            whole.add(material.mDiffuseNeverSolid);
        }

        whole.add(scene.getLayers());
        whole.add(scene.getMasks());

        // By their paths and by their slots both, which is the difference from `digestScene`: which
        // slot a texture landed in is what a material's index means.
        for (const VFS::Path::Normalized& texture : scene.getTextures())
        {
            const std::string_view path = texture.value();
            whole.add(std::span<const char>(path.data(), path.size()));
        }

        whole.add(scene.getLights());
        whole.add(scene.getSprites());

        for (const SpriteEmitter& emitter : scene.getEmitters())
        {
            whole.add(emitter.mCentre);
            whole.add(emitter.mReach);
            whole.add(emitter.mFirst);
            whole.add(emitter.mCount);
            whole.add(emitter.mTexture);
            whole.add(emitter.mLighting);
            whole.add(emitter.mAdditive);
            whole.add(emitter.mWidth);
        }

        // What poses a mesh that deforms, and the pose itself. The trace reads the posed vertices,
        // which live on the device and nowhere here, so these are what stands for them.
        whole.add(scene.getRigs());
        whole.add(scene.getRuns());
        whole.add(scene.getInfluences());
        whole.add(scene.getMorphs());
        whole.add(scene.getMorphOffsets());
        whole.add(scene.getBones());
        whole.add(scene.getWeights());

        return whole.getWords();
    }
}

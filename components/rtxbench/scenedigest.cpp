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

    /// The tables `digestParts` reads whole, held to having nothing between their fields.
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

    std::string_view nameOf(const ScenePart part)
    {
        switch (part)
        {
            case ScenePart::Positions:
                return "positions";
            case ScenePart::Normals:
                return "normals";
            case ScenePart::TexCoords:
                return "texcoords";
            case ScenePart::Indices:
                return "indices";
            case ScenePart::Meshes:
                return "meshes";
            case ScenePart::Instances:
                return "instances";
            case ScenePart::Previous:
                return "previous";
            case ScenePart::Materials:
                return "materials";
            case ScenePart::Layers:
                return "layers";
            case ScenePart::Masks:
                return "masks";
            case ScenePart::Textures:
                return "textures";
            case ScenePart::Lights:
                return "lights";
            case ScenePart::Sprites:
                return "sprites";
            case ScenePart::Emitters:
                return "emitters";
            case ScenePart::Rigs:
                return "rigs";
            case ScenePart::Morphs:
                return "morphs";
            case ScenePart::Bones:
                return "bones";
            case ScenePart::Count:
                break;
        }

        return "no such part";
    }

    ScenePartDigests digestParts(const SceneDesc& scene)
    {
        ScenePartDigests parts{};
        Digest one;

        const auto take = [&](const ScenePart part) {
            parts[static_cast<std::size_t>(part)] = one.getWords();
            one = Digest();
        };

        one.add(scene.getPositions());
        take(ScenePart::Positions);

        one.add(scene.getNormals());
        take(ScenePart::Normals);

        one.add(scene.getTexCoords());
        take(ScenePart::TexCoords);

        one.add(scene.getIndices());
        take(ScenePart::Indices);

        // **Every slot, standing or free.** A free one keeps the room and the offsets its last
        // occupant left, so it is part of the state a run has to repeat — and a slot order that
        // moved is exactly what `digestScene` sums away.
        for (const MeshRange& mesh : scene.getMeshes())
        {
            one.add(mesh.mVertexOffset);
            one.add(mesh.mVertexCount);
            one.add(mesh.mIndexOffset);
            one.add(mesh.mIndexCount);
            one.add(mesh.mShape.mSheet);
            one.add(mesh.mShape.mClosed);
            one.add(mesh.mDeform);
            one.add(mesh.mDeformer);
            one.add(mesh.mMaterial);
            one.add(mesh.mBindOffset);
            one.add(mesh.mPoseOffset);
            one.add(mesh.mPosed);
            one.add(mesh.mBounds._min);
            one.add(mesh.mBounds._max);
        }
        take(ScenePart::Meshes);

        for (const MeshInstance& instance : scene.getInstances())
        {
            one.add(std::span<const float>(instance.mTransform.ptr(), 16));
            one.add(instance.mMesh);
            one.add(instance.mMaterial);
            one.add(instance.mOpacity);
            one.add(instance.mFirstPerson);
        }
        take(ScenePart::Instances);

        one.add(scene.getPrevious());
        take(ScenePart::Previous);

        for (const Material& material : scene.getMaterials())
        {
            one.add(material.mKind);
            one.add(material.mDiffuse);
            one.add(material.mNormal);
            one.add(material.mEmissive);
            one.add(material.mDiffuseColour);
            one.add(material.mEmissiveColour);
            one.add(material.mAlphaRef);
            one.add(material.mAlphaMode);
            one.add(material.mTwoSided);
            one.add(material.mTextureTransform);
            one.add(material.mLayerOffset);
            one.add(material.mLayerCount);
            one.add(material.mFlatten);
            one.add(material.mAnimated);
            one.add(material.mDiffuseNeverSolid);
        }
        take(ScenePart::Materials);

        one.add(scene.getLayers());
        take(ScenePart::Layers);

        one.add(scene.getMasks());
        take(ScenePart::Masks);

        // By their paths and by their slots both, which is the difference from `digestScene`: which
        // slot a texture landed in is what a material's index means.
        for (const VFS::Path::Normalized& texture : scene.getTextures())
        {
            const std::string_view path = texture.value();
            one.add(std::span<const char>(path.data(), path.size()));
        }
        take(ScenePart::Textures);

        one.add(scene.getLights());
        take(ScenePart::Lights);

        one.add(scene.getSprites());
        take(ScenePart::Sprites);

        for (const SpriteEmitter& emitter : scene.getEmitters())
        {
            one.add(emitter.mCentre);
            one.add(emitter.mReach);
            one.add(emitter.mFirst);
            one.add(emitter.mCount);
            one.add(emitter.mTexture);
            one.add(emitter.mLighting);
            one.add(emitter.mAdditive);
            one.add(emitter.mWidth);
        }
        take(ScenePart::Emitters);

        // What poses a mesh that deforms, and the pose itself. The trace reads the posed vertices,
        // which live on the device and nowhere here, so these are what stands for them.
        one.add(scene.getRigs());
        one.add(scene.getRuns());
        one.add(scene.getInfluences());
        take(ScenePart::Rigs);

        one.add(scene.getMorphs());
        one.add(scene.getMorphOffsets());
        take(ScenePart::Morphs);

        one.add(scene.getBones());
        one.add(scene.getWeights());
        take(ScenePart::Bones);

        return parts;
    }

    std::array<std::uint64_t, 2> digestLayout(const ScenePartDigests& parts)
    {
        Digest whole;
        for (const std::array<std::uint64_t, 2>& part : parts)
            whole.add(part);

        return whole.getWords();
    }
}

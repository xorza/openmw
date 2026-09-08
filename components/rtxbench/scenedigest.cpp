#include "scenedigest.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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

        /// Hands every field of `material` to one of three callables.
        ///
        /// **One list and three kinds, because the two digests spell two of the kinds
        /// differently.** A texture reaches `digestParts` as the slot it landed in, which is what a
        /// material's index means, and reaches `digestScene` as the file it names, which is what
        /// the same material is wherever the slots fell. A run reaches the first whole, offset
        /// included, and the second by its length alone — an offset is where a chunk's layers were
        /// put and not what they are.
        ///
        /// **A field added to `Material` and not named here does not compile**, which is the whole
        /// of why this exists: the two lists it replaces were kept by hand, held different subsets,
        /// and a field added to neither would have left the gate quietly.
        /// `ExtractionStats::countersOf` makes the same argument for the same reason.
        template <class Texture, class Layers, class Value>
        void forEachMaterialField(const Material& material, Texture texture, Layers layers, Value value)
        {
            const auto& [kind, diffuse, normal, emissive, diffuseColour, emissiveColour, alphaRef, alphaMode, twoSided,
                textureTransform, run, flatten, animated, neverSolid]
                = material;

            texture(diffuse);
            texture(normal);
            texture(emissive);

            layers(run);

            value(kind);
            value(diffuseColour);
            value(emissiveColour);
            value(alphaRef);
            value(alphaMode);
            value(twoSided);
            value(textureTransform);
            value(flatten);
            value(animated);
            value(neverSolid);
        }

        void addMaterial(Digest& digest, const SceneDesc& scene, const Index index)
        {
            digest.add(index == sNoIndex);
            if (index == sNoIndex)
                return;

            const Material& material = scene.getMaterials()[index];
            forEachMaterialField(
                material, [&](const Index slot) { addTexture(digest, scene, slot); },
                [&](const Run& layers) { digest.add(layers.mCount); }, [&](const auto& field) { digest.add(field); });

            for (const Rtx::MaterialLayer& layer : material.mLayers.in(scene.getLayers()))
            {
                addTexture(digest, scene, layer.mDiffuse);
                digest.add(layer.mDiffuseTransform);
                digest.add(layer.mMaskTransform);
                digest.add(layer.mMask.in(scene.getMasks()));
            }
        }

        /// Every field of one row, as one list.
        ///
        /// **A field added and not named here does not compile.** `digestParts` reads these whole,
        /// so a field it did not name would be one the gate stopped watching — silently, and on
        /// the one report every determinism argument in this fork rests on.
        auto fieldsOf(const MeshRange& mesh)
        {
            const auto& [vertices, indices, shape, deform, deformer, material, bindOffset, poseOffset, posed, bounds]
                = mesh;
            return std::tie(
                vertices, indices, shape, deform, deformer, material, bindOffset, poseOffset, posed, bounds);
        }

        auto fieldsOf(const MeshInstance& instance)
        {
            const auto& [transform, mesh, material, opacity, firstPerson] = instance;
            return std::tie(transform, mesh, material, opacity, firstPerson);
        }

        auto fieldsOf(const SpriteEmitter& emitter)
        {
            const auto& [centre, reach, sprites, texture, lighting, additive, width] = emitter;
            return std::tie(centre, reach, sprites, texture, lighting, additive, width);
        }

        void addFields(Digest& digest, const auto& fields)
        {
            std::apply([&digest](const auto&... field) { (digest.add(field), ...); }, fields);
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
            const std::span<const std::uint32_t> indices = mesh.mIndices.in(scene.getIndices());
            for (std::size_t at = 0; at + 2 < indices.size(); at += 3)
            {
                std::array<Corner, 3> corners;
                for (std::size_t corner = 0; corner < 3; ++corner)
                {
                    const std::size_t vertex = mesh.mVertices.mOffset + indices[at + corner];
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
            for (const Rtx::Sprite& sprite : emitter.mSprites.in(scene.getSprites()))
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
    static_assert(sizeof(MaterialLayer) == 48, "MaterialLayer is read whole and must have no padding");
    static_assert(sizeof(Rig) == 24, "Rig is read whole and must have no padding");
    static_assert(sizeof(Morph) == 16, "Morph is read whole and must have no padding");
    static_assert(sizeof(Shaders::GpuBone) == 48, "GpuBone is read whole and must have no padding");
    static_assert(sizeof(Shaders::GpuInfluence) == 8, "GpuInfluence is read whole and must have no padding");

    /// The same, for the field types the lists above hand over as one value each.
    static_assert(sizeof(Run) == 8, "Run is read whole and must have no padding");
    static_assert(sizeof(FoldedShape) == 2, "FoldedShape is read whole and must have no padding");
    static_assert(sizeof(osg::BoundingBoxf) == 24, "a bounding box is read whole and must have no padding");
    static_assert(sizeof(osg::Matrixf) == 64, "a transform is read whole and must have no padding");

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
            addFields(one, fieldsOf(mesh));
        take(ScenePart::Meshes);

        for (const MeshInstance& instance : scene.getInstances())
            addFields(one, fieldsOf(instance));
        take(ScenePart::Instances);

        one.add(scene.getPrevious());
        take(ScenePart::Previous);

        // The slot a texture landed in and the offset a layer run was placed at, because that is
        // what this digest is for: which table a material points into is what a layout is.
        for (const Material& material : scene.getMaterials())
            forEachMaterialField(
                material, [&](const Index slot) { one.add(slot); }, [&](const Run& layers) { one.add(layers); },
                [&](const auto& field) { one.add(field); });
        take(ScenePart::Materials);

        one.add(scene.getLayers());
        take(ScenePart::Layers);

        one.add(scene.getMasks());
        take(ScenePart::Masks);

        // By their names and by their slots both, which is the difference from `digestScene`: which
        // slot a texture landed in is what a material's index means.
        //
        // **The baked names beside the paths, because a slot is one or the other.** A texture this
        // renderer made has no path, so a column of paths alone reads every baked slot as the same
        // empty string — and a run whose bakes landed in another order came out identical here
        // while the materials naming them moved. Measured on `one-cell-walk`: `mDiffuse` differed
        // on 5 frames of 6 with this column agreeing on all of them.
        const std::span<const VFS::Path::Normalized> paths = scene.getTextures();
        const std::span<const std::string> baked = scene.getBakedTextures();
        assert(paths.size() == baked.size() && "a texture table whose two names disagree on how many slots it has");

        for (std::size_t slot = 0; slot < paths.size(); ++slot)
        {
            const std::string_view path = paths[slot].value();
            one.add(std::span<const char>(path.data(), path.size()));

            one.add(std::span<const char>(baked[slot].data(), baked[slot].size()));
        }
        take(ScenePart::Textures);

        one.add(scene.getLights());
        take(ScenePart::Lights);

        one.add(scene.getSprites());
        take(ScenePart::Sprites);

        for (const SpriteEmitter& emitter : scene.getEmitters())
            addFields(one, fieldsOf(emitter));
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

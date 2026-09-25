#include "scenedigest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <osg/BoundingBox>
#include <osg/Matrixf>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <smhasher/MurmurHash3.h>

#include <components/rtx/deformertable.hpp>
#include <components/rtx/lightbuilder.hpp>
#include <components/rtx/material.hpp>
#include <components/rtx/mesh.hpp>
#include <components/rtx/ripple.hpp>
#include <components/rtx/runs.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/shaders/skinning.h>
#include <components/rtx/shapefold.hpp>
#include <components/rtx/sprite.hpp>
#include <components/rtx/textureencoding.hpp>
#include <components/rtx/texturetable.hpp>
#include <components/vfs/pathutil.hpp>

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

            const TextureRow& row = scene.textures().getRows()[texture];
            const std::string_view path = row.mPath.value();
            digest.add(std::span<const char>(path.data(), path.size()));
            digest.add(row.mWrap);

            // Only where it is not a colour, so a scene of colours digests as it did before a slot had
            // an encoding.
            if (row.mEncoding != TextureEncoding::Colour)
                digest.add(row.mEncoding);
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
        /// `sCounters` in `extractionstats.cpp` makes the same argument for the same reason.
        template <class Texture, class Layers, class Value>
        void forEachMaterialField(const Material& material, Texture texture, Layers layers, Value value)
        {
            const auto& [kind, diffuse, emissive, environment, environmentColour, dark, darkUnit, normal, specular,
                parallax, diffuseColour, emissiveColour, opacity, alphaRef, alphaMode, blend, vertexColour, twoSided,
                textureTransform, run, flatten, layersMapped, animated, neverSolid, diffuseMean]
                = material;

            texture(diffuse);
            texture(emissive);
            texture(environment);
            texture(dark);

            // The companion maps only where there are any, each behind its own tag, so a scene with
            // none digests as it did before a material could have them.
            if (normal != sNoIndex)
            {
                value(std::uint8_t{ 1 });
                texture(normal);
            }
            if (specular != sNoIndex)
            {
                value(std::uint8_t{ 2 });
                texture(specular);
            }
            if (layersMapped)
                value(std::uint8_t{ 3 });
            if (parallax)
                value(std::uint8_t{ 4 });

            layers(run);

            value(kind);
            value(environmentColour);
            value(darkUnit);
            value(diffuseColour);
            value(emissiveColour);
            value(opacity);
            value(alphaRef);
            value(alphaMode);
            value(blend);
            value(vertexColour);
            value(twoSided);
            value(textureTransform);
            value(flatten);
            value(animated);
            value(neverSolid);
            value(diffuseMean);
        }

        void addMaterial(Digest& digest, const SceneDesc& scene, const Index index)
        {
            digest.add(index == sNoIndex);
            if (index == sNoIndex)
                return;

            const Material& material = scene.materials().getRows()[index];
            forEachMaterialField(
                material, [&](const Index slot) { addTexture(digest, scene, slot); },
                [&](const Run& layers) { digest.add(layers.mCount); }, [&](const auto& field) { digest.add(field); });

            for (const Rtx::MaterialLayer& layer : material.mLayers.in(scene.materials().getLayers()))
            {
                addTexture(digest, scene, layer.mDiffuse);
                digest.add(layer.mDiffuseTransform);
                digest.add(layer.mMaskTransform);
                digest.add(maskOf(layer).in(scene.materials().getMasks()));

                // Only where a layer has them, each behind its tag, as a material's companion maps.
                if (layer.mNormal != sNoIndex)
                {
                    digest.add(std::uint8_t{ 1 });
                    addTexture(digest, scene, layer.mNormal);
                }
                if (layer.mFlags != 0)
                {
                    digest.add(std::uint8_t{ 2 });
                    digest.add(layer.mFlags);
                }
            }
        }

        /// Every field of one row, as one list.
        ///
        /// **A field added and not named here does not compile.** `digestParts` reads these whole,
        /// so a field it did not name would be one the gate stopped watching — silently, and on
        /// the one report every determinism argument in this fork rests on.
        ///
        /// **Whether it brought tangents is the one field left out**, and the meshes column adds it
        /// where it is set: a scene where no mesh has any digests to the words on record.
        auto fieldsOf(const MeshRange& mesh)
        {
            const auto& [vertices, indices, secondTexCoords, unitStreams, tangents, shape, deform, deformer, bindOffset,
                poseOffset, posed, bounds]
                = mesh;
            return std::tie(vertices, indices, secondTexCoords, unitStreams, shape, deform, deformer, bindOffset,
                poseOffset, posed, bounds);
        }

        auto fieldsOf(const MeshInstance& instance)
        {
            const auto& [transform, mesh, material, opacity, instanceClass, stander] = instance;
            return std::tie(transform, mesh, material, opacity, instanceClass, stander);
        }

        /// Field by field, because the kind is a byte and the row carries padding after it.
        auto fieldsOf(const Deformer& deformer)
        {
            const auto& [kind, runs, influences, offsets, rows] = deformer;
            return std::tie(kind, runs, influences, offsets, rows);
        }

        void addFields(Digest& digest, const auto& fields)
        {
            std::apply([&digest](const auto&... field) { (digest.add(field), ...); }, fields);
        }

        /// One table hashed where it lies.
        template <class T>
        std::array<std::uint64_t, 2> wordsOf(const std::span<const T> table)
        {
            Digest whole;
            whole.add(table);
            return whole.getWords();
        }

        /// A table in blocks hashed where it lies, a block at a time. A table of one block or none
        /// is hashed as the one span it would be laid out flat, so its words are the flat table's;
        /// past one block the spans chain through the seed, which no flat hash can say.
        template <class T>
        void addBlocks(Digest& digest, const BlockedValues<T>& table)
        {
            if (table.size() == 0)
                digest.add(std::span<const T>());
            table.forEachBlock([&](const std::span<const T> block) { digest.add(block); });
        }

        template <class T>
        std::array<std::uint64_t, 2> wordsOf(const BlockedValues<T>& table)
        {
            Digest whole;
            addBlocks(whole, table);
            return whole.getWords();
        }

        /// A column of rows: every field of every row laid end to end in one buffer, so the column
        /// is one hash over its bytes and not one hash per field.
        class Column
        {
        public:
            explicit Column(std::vector<std::byte>& scratch)
                : mScratch(scratch)
            {
                mScratch.clear();
            }

            template <class T>
            void add(const T& value)
            {
                const auto bytes = std::as_bytes(std::span<const T>(&value, 1));
                mScratch.insert(mScratch.end(), bytes.begin(), bytes.end());
            }

            template <class T>
            void add(const std::span<const T> values)
            {
                const auto bytes = std::as_bytes(values);
                mScratch.insert(mScratch.end(), bytes.begin(), bytes.end());
            }

            void addFields(const auto& fields)
            {
                std::apply([this](const auto&... field) { (add(field), ...); }, fields);
            }

            /// The column's digest, over everything added.
            std::array<std::uint64_t, 2> take() const { return wordsOf(std::span<const std::byte>(mScratch)); }

        private:
            std::vector<std::byte>& mScratch;
        };

        /// One corner of a triangle, as the picture sees it.
        struct Corner
        {
            osg::Vec3f mPosition;
            osg::Vec3f mNormal;
            osg::Vec2f mTexCoord;
            osg::Vec3f mColour;
            std::uint32_t mTangent = 0;

            bool operator<(const Corner& other) const
            {
                return std::tie(mPosition, mNormal, mTexCoord, mColour, mTangent)
                    < std::tie(other.mPosition, other.mNormal, other.mTexCoord, other.mColour, other.mTangent);
            }
        };

        void addCorner(Digest& digest, const Corner& corner)
        {
            digest.add(corner.mPosition);
            digest.add(corner.mNormal);
            digest.add(corner.mTexCoord);
            digest.add(corner.mColour);

            // Only where there is one, so a corner with none digests to the words on record.
            if (corner.mTangent != 0)
                digest.add(corner.mTangent);
        }

        /// A shape as the multiset of its triangles, each turned to start at its least corner so
        /// that the winding survives and the corner it happens to be spelt from does not.
        Unordered digestTriangles(const SceneDesc& scene, const MeshRange& mesh)
        {
            Unordered triangles;
            const std::span<const std::uint32_t> indices = mesh.mIndices.in(scene.meshes().getIndices());
            for (std::size_t at = 0; at + 2 < indices.size(); at += 3)
            {
                std::array<Corner, 3> corners;
                for (std::size_t corner = 0; corner < 3; ++corner)
                {
                    const std::uint32_t vertex = mesh.mVertices.mOffset + indices[at + corner];
                    corners[corner] = Corner{ scene.meshes().getPositions()[vertex],
                        scene.meshes().getNormals()[vertex], scene.meshes().getTexCoords()[vertex],
                        scene.meshes().getColours()[vertex], scene.meshes().getTangents()[vertex] };
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
            const MeshRange& mesh = scene.meshes().getRows()[index];
            digest.add(digestTriangles(scene, mesh).getWords());
            digest.add(mesh.mDeform);
        }
    }

    std::array<std::uint64_t, 2> digestScene(const SceneDesc& scene)
    {
        Unordered whole;

        for (const Rtx::PlacementRow& row : scene.placements().getRows())
        {
            const Rtx::MeshInstance& instance = row.mInstance;
            if (instance.mMesh == sNoIndex)
                continue;

            Digest placement;
            placement.add(std::span<const float>(instance.mTransform.ptr(), 16));
            placement.add(instance.mOpacity);
            placement.add(static_cast<std::uint32_t>(instance.mClass));
            addMaterial(placement, scene, instance.mMaterial);
            addMesh(placement, scene, instance.mMesh);
            whole.add(placement);
        }

        for (const Rtx::Light& light : scene.lights())
        {
            Digest lamp;
            lamp.add(light.mPosition);
            lamp.add(light.mIntensity);
            lamp.add(light.mReach);
            whole.add(lamp);
        }

        for (const Rtx::SpriteEmitter& emitter : scene.emitters())
        {
            Digest plume;
            plume.add(emitter.mCentre);
            plume.add(emitter.mReach);
            plume.add(emitter.isAdditive());
            addTexture(plume, scene, emitter.mTexture);
            const Rtx::Run run = spritesOf(emitter);
            for (const Rtx::Sprite& sprite : run.in(scene.sprites()))
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
    static_assert(sizeof(Light) == 40, "Light is read whole and must have no padding");
    static_assert(sizeof(Sprite) == 56, "Sprite is read whole and must have no padding");
    static_assert(sizeof(SpriteEmitter) == 40, "SpriteEmitter is read whole and must have no padding");
    static_assert(sizeof(Shaders::GpuBone) == 48, "GpuBone is read whole and must have no padding");
    static_assert(sizeof(Shaders::GpuInfluence) == 8, "GpuInfluence is read whole and must have no padding");

    /// The same, for the field types the lists above hand over as one value each.
    static_assert(sizeof(Run) == 8, "Run is read whole and must have no padding");
    static_assert(sizeof(FoldedShape) == 3, "FoldedShape is read whole and must have no padding");
    static_assert(sizeof(osg::BoundingBoxf) == 24, "a bounding box is read whole and must have no padding");
    static_assert(sizeof(osg::Matrixf) == 64, "a transform is read whole and must have no padding");

    void SceneDigester::digestVertices(const SceneDesc& scene)
    {
        const MeshTable& meshes = scene.meshes();
        const Vertices now{
            .mRevision = meshes.getRevision(),
            .mLengths = { meshes.getPositions().size(), meshes.getNormals().size(), meshes.getTexCoords().size(),
                meshes.getSecondTexCoords().size(), meshes.getIndices().size() },
        };
        if (mVertices == now)
            return;

        take(ScenePart::Positions, wordsOf(meshes.getPositions()));

        // **The tangents are the normals' part, and only where a mesh has any**, so a scene where
        // no vertex has one digests to the words on record, and a report keeps its columns.
        Digest normals;
        addBlocks(normals, meshes.getNormals());
        bool tangents = false;
        meshes.getTangents().forEachBlock([&](const std::span<const std::uint32_t> block) {
            tangents = tangents || std::ranges::any_of(block, [](const std::uint32_t word) { return word != 0; });
        });
        if (tangents)
            addBlocks(normals, meshes.getTangents());
        take(ScenePart::Normals, normals.getWords());

        Digest texCoords;
        addBlocks(texCoords, meshes.getTexCoords());
        addBlocks(texCoords, meshes.getSecondTexCoords());
        take(ScenePart::TexCoords, texCoords.getWords());

        take(ScenePart::Indices, wordsOf(meshes.getIndices()));

        mVertices = now;
        ++mVertexHashes;
    }

    const ScenePartDigests& SceneDigester::digest(const SceneDesc& scene, const Shaders::VisibilityConstants* frame)
    {
        digestVertices(scene);

        // **Every slot, standing or free.** A free one keeps the room and the offsets its last
        // occupant left, so it is part of the state a run has to repeat — and a slot order that
        // moved is exactly what `digestScene` sums away.
        Column meshes(mScratch);
        for (const MeshRange& mesh : scene.meshes().getRows())
        {
            meshes.addFields(fieldsOf(mesh));
            if (mesh.mTangents)
                meshes.add(mesh.mTangents);
        }
        take(ScenePart::Meshes, meshes.take());

        Column instances(mScratch);
        for (const PlacementRow& row : scene.placements().getRows())
            instances.addFields(fieldsOf(row.mInstance));
        take(ScenePart::Instances, instances.take());

        Column previous(mScratch);
        for (const PlacementRow& row : scene.placements().getRows())
            previous.add(row.mPrevious);
        take(ScenePart::Previous, previous.take());

        // The slot a texture landed in and the offset a layer run was placed at, because that is
        // what this digest is for: which table a material points into is what a layout is.
        Column materials(mScratch);
        for (const Material& material : scene.materials().getRows())
            forEachMaterialField(
                material, [&](const Index slot) { materials.add(slot); },
                [&](const Run& layers) { materials.add(layers); }, [&](const auto& field) { materials.add(field); });
        take(ScenePart::Materials, materials.take());

        // **Each row as the 48 bytes it was before it could name a normal map, and the two fields
        // since only where a layer sets them**, so a scene whose ground has no map digests to the
        // words on record. Bound whole, so a field added and not named here does not compile; the
        // padding is no field.
        Column layers(mScratch);
        for (const MaterialLayer& layer : scene.materials().getLayers())
        {
            const auto& [diffuse, maskOffset, maskWidth, maskHeight, diffuseTransform, maskTransform, normal, flags,
                padding]
                = layer;
            layers.addFields(std::tie(diffuse, maskOffset, maskWidth, maskHeight, diffuseTransform, maskTransform));
            if (normal != sNoIndex)
                layers.addFields(std::tuple(std::uint8_t{ 1 }, normal));
            if (flags != 0)
                layers.addFields(std::tuple(std::uint8_t{ 2 }, flags));
        }
        take(ScenePart::Layers, layers.take());
        take(ScenePart::Masks, wordsOf(scene.materials().getMasks()));

        // By their names and by their slots both, which is the difference from `digestScene`: which
        // slot a texture landed in is what a material's index means.
        //
        // **The baked names beside the paths, because a slot is one or the other.** A texture this
        // renderer made has no path, so a column of paths alone reads every baked slot as the same
        // empty string — and a run whose bakes landed in another order comes out identical here
        // while the materials naming them move. Each name ends with its length, so two names laid
        // end to end cannot be read as one.
        Column textures(mScratch);
        for (const TextureRow& row : scene.textures().getRows())
        {
            const std::string_view path = row.mPath.value();
            textures.add(std::span<const char>(path.data(), path.size()));
            textures.add(static_cast<std::uint32_t>(path.size()));

            textures.add(std::span<const char>(row.mBaked.data(), row.mBaked.size()));
            textures.add(static_cast<std::uint32_t>(row.mBaked.size()));
        }
        take(ScenePart::Textures, textures.take());

        take(ScenePart::Lights, wordsOf(scene.lights()));
        take(ScenePart::Sprites, wordsOf(scene.sprites()));
        take(ScenePart::Emitters, wordsOf(scene.emitters()));

        Column ripples(mScratch);
        for (const RippleImpulse& impulse : scene.ripples())
        {
            ripples.add(impulse.mAt);
            ripples.add(impulse.mSize);
        }
        take(ScenePart::Ripples, ripples.take());

        // What poses a mesh that deforms, and the pose itself. The trace reads the posed vertices,
        // which live on the device and nowhere here, so these are what stands for them.
        Column deformers(mScratch);
        for (const Deformer& deformer : scene.deformers().getDeformers())
            deformers.addFields(fieldsOf(deformer));
        deformers.add(scene.deformers().getRuns());
        deformers.add(scene.deformers().getInfluences());
        deformers.add(scene.deformers().getMorphOffsets());
        take(ScenePart::Deformers, deformers.take());

        take(ScenePart::Poses, wordsOf(scene.deformers().getPoses()));

        // Whole, because the block is scalar-packed on every side, which `visibility.h` pins.
        take(ScenePart::Frame,
            frame != nullptr ? wordsOf(std::span<const Shaders::VisibilityConstants>(frame, 1))
                             : std::array<std::uint64_t, 2>{});

        return mParts;
    }

    ScenePartDigests digestParts(const SceneDesc& scene)
    {
        SceneDigester once;
        return once.digest(scene);
    }

    std::array<std::uint64_t, 2> digestLayout(const ScenePartDigests& parts)
    {
        Digest whole;
        for (const std::array<std::uint64_t, 2>& part : parts)
            whole.add(part);

        return whole.getWords();
    }

    void Digest::add(std::span<const std::byte> bytes)
    {
        // The seed is read whole before anything is written, but a copy costs two words and makes
        // that true whatever the implementation does.
        const std::array<std::uint64_t, 2> seed = mWords;
        MurmurHash3_x64_128(bytes.data(), static_cast<int>(bytes.size()), seed.data(), mWords.data());
    }

    std::string spellHash(const std::array<std::uint64_t, 2>& words)
    {
        return std::format("{:016x}{:016x}", words[0], words[1]);
    }

}

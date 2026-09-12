#include "framehashes.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <osg/Vec2f>
#include <osg/Vec3f>
#include <smhasher/MurmurHash3.h>

#include <components/files/conversion.hpp>
#include <components/rtx/error.hpp>
#include <components/rtx/scenedesc.hpp>
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

            const std::string_view path = scene.textures().getPaths()[texture].value();
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
            const auto& [kind, diffuse, normal, emissive, diffuseColour, emissiveColour, opacity, alphaRef, alphaMode,
                vertexColour, twoSided, textureTransform, run, flatten, animated, neverSolid]
                = material;

            texture(diffuse);
            texture(normal);
            texture(emissive);

            layers(run);

            value(kind);
            value(diffuseColour);
            value(emissiveColour);
            value(opacity);
            value(alphaRef);
            value(alphaMode);
            value(vertexColour);
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

            const Material& material = scene.materials().getRows()[index];
            forEachMaterialField(
                material, [&](const Index slot) { addTexture(digest, scene, slot); },
                [&](const Run& layers) { digest.add(layers.mCount); }, [&](const auto& field) { digest.add(field); });

            for (const Rtx::MaterialLayer& layer : material.mLayers.in(scene.materials().getLayers()))
            {
                addTexture(digest, scene, layer.mDiffuse);
                digest.add(layer.mDiffuseTransform);
                digest.add(layer.mMaskTransform);
                digest.add(layer.mMask.in(scene.materials().getMasks()));
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
            osg::Vec3f mColour;

            bool operator<(const Corner& other) const
            {
                return std::tie(mPosition, mNormal, mTexCoord, mColour)
                    < std::tie(other.mPosition, other.mNormal, other.mTexCoord, other.mColour);
            }
        };

        void addCorner(Digest& digest, const Corner& corner)
        {
            digest.add(corner.mPosition);
            digest.add(corner.mNormal);
            digest.add(corner.mTexCoord);
            digest.add(corner.mColour);
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
                    const std::size_t vertex = mesh.mVertices.mOffset + indices[at + corner];
                    corners[corner]
                        = Corner{ scene.meshes().getPositions()[vertex], scene.meshes().getNormals()[vertex],
                              scene.meshes().getTexCoords()[vertex], scene.meshes().getColours()[vertex] };
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

        for (const Rtx::MeshInstance& instance : scene.placements().getAll())
        {
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
            plume.add(emitter.mAdditive);
            addTexture(plume, scene, emitter.mTexture);
            for (const Rtx::Sprite& sprite : emitter.mSprites.in(scene.sprites()))
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
    static_assert(sizeof(Rig) == 20, "Rig is read whole and must have no padding");
    static_assert(sizeof(Morph) == 12, "Morph is read whole and must have no padding");
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

        one.add(scene.meshes().getPositions());
        take(ScenePart::Positions);

        one.add(scene.meshes().getNormals());
        take(ScenePart::Normals);

        one.add(scene.meshes().getTexCoords());
        take(ScenePart::TexCoords);

        one.add(scene.meshes().getIndices());
        take(ScenePart::Indices);

        // **Every slot, standing or free.** A free one keeps the room and the offsets its last
        // occupant left, so it is part of the state a run has to repeat — and a slot order that
        // moved is exactly what `digestScene` sums away.
        for (const MeshRange& mesh : scene.meshes().getRows())
            addFields(one, fieldsOf(mesh));
        take(ScenePart::Meshes);

        for (const MeshInstance& instance : scene.placements().getAll())
            addFields(one, fieldsOf(instance));
        take(ScenePart::Instances);

        one.add(scene.placements().getPrevious());
        take(ScenePart::Previous);

        // The slot a texture landed in and the offset a layer run was placed at, because that is
        // what this digest is for: which table a material points into is what a layout is.
        for (const Material& material : scene.materials().getRows())
            forEachMaterialField(
                material, [&](const Index slot) { one.add(slot); }, [&](const Run& layers) { one.add(layers); },
                [&](const auto& field) { one.add(field); });
        take(ScenePart::Materials);

        one.add(scene.materials().getLayers());
        take(ScenePart::Layers);

        one.add(scene.materials().getMasks());
        take(ScenePart::Masks);

        // By their names and by their slots both, which is the difference from `digestScene`: which
        // slot a texture landed in is what a material's index means.
        //
        // **The baked names beside the paths, because a slot is one or the other.** A texture this
        // renderer made has no path, so a column of paths alone reads every baked slot as the same
        // empty string — and a run whose bakes landed in another order comes out identical here
        // while the materials naming them move.
        const std::span<const VFS::Path::Normalized> paths = scene.textures().getPaths();
        const std::span<const std::string> baked = scene.textures().getBaked();
        assert(paths.size() == baked.size() && "a texture table whose two names disagree on how many slots it has");

        for (std::size_t slot = 0; slot < paths.size(); ++slot)
        {
            const std::string_view path = paths[slot].value();
            one.add(std::span<const char>(path.data(), path.size()));

            one.add(std::span<const char>(baked[slot].data(), baked[slot].size()));
        }
        take(ScenePart::Textures);

        one.add(scene.lights());
        take(ScenePart::Lights);

        one.add(scene.sprites());
        take(ScenePart::Sprites);

        for (const SpriteEmitter& emitter : scene.emitters())
            addFields(one, fieldsOf(emitter));
        take(ScenePart::Emitters);

        // What poses a mesh that deforms, and the pose itself. The trace reads the posed vertices,
        // which live on the device and nowhere here, so these are what stands for them.
        one.add(scene.deformers().getRigs());
        one.add(scene.deformers().getRuns());
        one.add(scene.deformers().getInfluences());
        take(ScenePart::Rigs);

        one.add(scene.deformers().getMorphs());
        one.add(scene.deformers().getMorphOffsets());
        take(ScenePart::Morphs);

        one.add(scene.deformers().getBones());
        one.add(scene.deformers().getWeights());
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

    namespace
    {
        /// How many differing frames a report names before it stops counting them out.
        constexpr std::size_t sNamed = 6;

        /// The columns before the parts: the view, the frame and the picture.
        constexpr std::size_t sNamedColumns = 3;

        constexpr std::size_t sColumns = sNamedColumns + static_cast<std::size_t>(ScenePart::Count);

        /// What a file opens with, and the one statement of what its columns are.
        std::string headerLine()
        {
            std::string header = "view,frame,picture";
            for (std::size_t part = 0; part < static_cast<std::size_t>(ScenePart::Count); ++part)
                header += ',' + std::string(nameOf(static_cast<ScenePart>(part)));

            return header;
        }

        /// Which parts moved and on how many frames, biggest first — or nothing where none did.
        std::string namePartsDiffering(const FrameHashes::ViewDifference& difference)
        {
            std::vector<std::size_t> moved;
            for (std::size_t part = 0; part < difference.mPartsDiffering.size(); ++part)
                if (difference.mPartsDiffering[part] > 0)
                    moved.push_back(part);

            if (moved.empty())
                return {};

            // Stable, so that parts that moved on as many frames read in the order a scene is laid
            // out rather than in whichever order the sort left them.
            std::stable_sort(moved.begin(), moved.end(), [&](const std::size_t left, const std::size_t right) {
                return difference.mPartsDiffering[left] > difference.mPartsDiffering[right];
            });

            std::string named = " — ";
            for (std::size_t at = 0; at < moved.size(); ++at)
                named += std::format("{}{} {}", at > 0 ? ", " : "", nameOf(static_cast<ScenePart>(moved[at])),
                    difference.mPartsDiffering[moved[at]]);

            return named;
        }
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

    void FrameHashes::add(const std::string_view view, const std::uint32_t frame, std::span<const std::uint8_t> pixels,
        const ScenePartDigests& parts)
    {
        Digest digest;
        digest.add(pixels);
        mFrames.push_back(
            Frame{ .mView = std::string(view), .mFrame = frame, .mHash = digest.getWords(), .mParts = parts });
    }

    void FrameHashes::write(const std::filesystem::path& file) const
    {
        std::ofstream out(file);
        out << headerLine() << '\n';

        for (const Frame& held : mFrames)
        {
            out << held.mView << ',' << held.mFrame << ',' << spellHash(held.mHash);
            for (const std::array<std::uint64_t, 2>& part : held.mParts)
                out << ',' << spellHash(part);

            out << '\n';
        }

        // **Thrown and not reported**, the way `shot --dump` answers the same failure: a reference
        // that did not get written and a command that still succeeded is the next run comparing
        // against whatever was at that path before.
        if (!out)
            throw Error("could not write " + Files::pathToUnicodeString(file));
    }

    FrameHashes FrameHashes::read(const std::filesystem::path& file)
    {
        std::ifstream in(file);
        if (!in)
            throw Error("could not read " + Files::pathToUnicodeString(file));

        const auto fail = [&](const std::string& line) {
            return Error("cannot read " + Files::pathToUnicodeString(file) + ": " + line);
        };

        std::string line;

        // **The header has to be this build's, exactly.** A file written before a column existed
        // would otherwise be compared column by column against one that has it, and every row would
        // read as a difference in a table nobody changed.
        if (!std::getline(in, line) || line != headerLine())
            throw fail(line);

        const auto readHash = [](const std::string_view field, std::array<std::uint64_t, 2>& into) {
            if (field.size() != 32)
                return false;

            for (int half = 0; half < 2; ++half)
            {
                const char* const from = field.data() + half * 16;
                if (std::from_chars(from, from + 16, into[half], 16).ec != std::errc{})
                    return false;
            }

            return true;
        };

        // Cleared and refilled a line at a time, rather than allocated per line of a file a run
        // reads in full.
        std::vector<std::string_view> fields;

        FrameHashes held;
        while (std::getline(in, line))
        {
            if (line.empty())
                continue;

            fields.clear();
            for (std::size_t at = 0; at <= line.size();)
            {
                const std::size_t comma = std::min(line.find(',', at), line.size());
                fields.push_back(std::string_view(line).substr(at, comma - at));
                at = comma + 1;
            }

            // **Every line or none.** A reference read half way is one that matches the frames it
            // reached and says nothing about the rest, which reads as a pass.
            if (fields.size() != sColumns)
                throw fail(line);

            Frame frame;
            frame.mView = std::string(fields[0]);
            if (std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), frame.mFrame).ec != std::errc{})
                throw fail(line);

            if (!readHash(fields[2], frame.mHash))
                throw fail(line);

            for (std::size_t part = 0; part < frame.mParts.size(); ++part)
                if (!readHash(fields[sNamedColumns + part], frame.mParts[part]))
                    throw fail(line);

            held.mFrames.push_back(std::move(frame));
        }

        return held;
    }

    std::vector<FrameHashes::ViewDifference> FrameHashes::against(const FrameHashes& reference) const
    {
        std::vector<ViewDifference> differences;

        for (const Frame& held : mFrames)
        {
            if (differences.empty() || differences.back().mView != held.mView)
                differences.push_back(ViewDifference{ .mView = held.mView });

            ViewDifference& difference = differences.back();
            ++difference.mFrames;

            const auto found = std::find_if(reference.mFrames.begin(), reference.mFrames.end(),
                [&](const Frame& was) { return was.mFrame == held.mFrame && was.mView == held.mView; });

            if (found == reference.mFrames.end())
            {
                ++difference.mUnmatched;
                continue;
            }

            if (found->mHash != held.mHash)
                difference.mDiffering.push_back(held.mFrame);

            bool anyPart = false;
            for (std::size_t part = 0; part < held.mParts.size(); ++part)
            {
                if (found->mParts[part] == held.mParts[part])
                    continue;

                ++difference.mPartsDiffering[part];
                anyPart = true;
            }

            if (anyPart)
                difference.mSceneDiffering.push_back(held.mFrame);
        }

        // **What the reference drew and this run did not**, which is a schedule that changed rather
        // than a picture that did: a run of fewer frames matches every frame it drew.
        for (ViewDifference& difference : differences)
        {
            const auto missing = std::count_if(reference.mFrames.begin(), reference.mFrames.end(),
                [&](const Frame& was) { return was.mView == difference.mView; });

            if (static_cast<std::uint32_t>(missing) > difference.mFrames)
                difference.mUnmatched += static_cast<std::uint32_t>(missing) - difference.mFrames;
        }

        return differences;
    }

    std::string describeDifference(const FrameHashes::ViewDifference& difference)
    {
        // **The scene is asked here too, though it does not fail the run.** Reporting only the
        // picture is what let a run be called identical while the description behind it moved on
        // every frame, which is the fault these columns were added for.
        if (difference.same() && difference.mSceneDiffering.empty())
            return std::format("{} frames, every one of them the same", difference.mFrames);

        std::string report;
        if (!difference.mDiffering.empty())
        {
            report = std::format("{} of {} frames differ, at ", difference.mDiffering.size(), difference.mFrames);
            for (std::size_t at = 0; at < std::min(sNamed, difference.mDiffering.size()); ++at)
                report += (at > 0 ? ", " : "") + std::to_string(difference.mDiffering[at]);

            if (difference.mDiffering.size() > sNamed)
                report += std::format(" and {} more", difference.mDiffering.size() - sNamed);
        }
        else if (!difference.mSceneDiffering.empty())
            report = std::format("{} frames, every picture the same", difference.mFrames);

        // **Which of the two moved, which is what says where to look next.** A picture that differs
        // where the scene differs is a world handed over twice, and belongs to whatever staged it.
        // One that differs where the scene did not is the renderer under it.
        if (!difference.mSceneDiffering.empty())
        {
            report += std::format("; the scene differs on {} frames", difference.mSceneDiffering.size());

            if (!difference.mDiffering.empty())
            {
                const auto both = std::count_if(
                    difference.mDiffering.begin(), difference.mDiffering.end(), [&](const std::uint32_t frame) {
                        return std::binary_search(
                            difference.mSceneDiffering.begin(), difference.mSceneDiffering.end(), frame);
                    });

                report += std::format(", {} of them among those", both);
            }

            // Last, because it is a list and anything appended after it would read as part of it.
            report += namePartsDiffering(difference);
        }
        else if (!difference.mDiffering.empty())
            report += "; the scene was the same on every frame";

        if (difference.mUnmatched > 0)
            report += std::format(
                "{}{} frames the two runs do not share", report.empty() ? "" : "; ", difference.mUnmatched);

        return report;
    }
}

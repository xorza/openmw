#include <cstdint>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Texture2D>
#include <osg/Vec2f>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/sceneutil/util.hpp>
#include <components/shader/automaps.hpp>
#include <components/testing/util.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

namespace Shader
{
    namespace
    {
        /// One 4 x 4 DXT1 block behind a DDS header: the smallest file the image manager reads.
        std::string ddsFile()
        {
            std::string bytes = "DDS ";
            const auto word = [&](std::uint32_t value) {
                for (int shift = 0; shift < 32; shift += 8)
                    bytes.push_back(static_cast<char>((value >> shift) & 0xFFu));
            };

            // Caps, height, width, pixel format and linear size; four by four; one block of eight.
            word(124);
            word(0x1u | 0x2u | 0x4u | 0x1000u | 0x80000u);
            word(4);
            word(4);
            word(8);
            word(0);
            word(0);
            for (int reserved = 0; reserved < 11; ++reserved)
                word(0);

            // The pixel format: a four-character code, and that code.
            word(32);
            word(0x4u);
            bytes += "DXT1";
            for (int mask = 0; mask < 5; ++mask)
                word(0);

            // A texture, and nothing past it.
            word(0x1000u);
            for (int caps = 0; caps < 4; ++caps)
                word(0);

            bytes.append(8, '\0');
            return bytes;
        }

        const AutoMapRules sEverything{
            .mNormalMaps = true,
            .mNormalMapPattern = "_n",
            .mNormalHeightMapPattern = "_nh",
            .mSpecularMaps = true,
            .mSpecularMapPattern = "_spec",
        };

        /// A content directory where `stone` has every companion, `wood` a normal map alone, and
        /// `sand` none.
        struct Content
        {
            TestingOpenMW::VFSTestFile mFile{ ddsFile() };
            std::unique_ptr<VFS::Manager> mVfs = TestingOpenMW::createTestVFS({
                { VFS::Path::NormalizedView("textures/stone_nh.dds"), &mFile },
                { VFS::Path::NormalizedView("textures/stone_n.dds"), &mFile },
                { VFS::Path::NormalizedView("textures/stone_spec.dds"), &mFile },
                { VFS::Path::NormalizedView("textures/wood_n.dds"), &mFile },
            });
            Resource::ImageManager mImages{ mVfs.get(), 0 };
        };

        /// A texture over an image that only carries `name`, which is all the rule reads of it.
        osg::ref_ptr<osg::Texture2D> named(const std::string& name)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setFileName(name);
            return new osg::Texture2D(image);
        }

        /// What `unit` holds: the file its texture was read from, and the type beside it.
        struct Bound
        {
            std::string mFile;
            std::string mType;
        };

        Bound boundAt(const osg::StateSet& stateSet, unsigned int unit)
        {
            const auto* texture
                = static_cast<const osg::Texture*>(stateSet.getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
            if (texture == nullptr || texture->getImage(0) == nullptr)
                return Bound{};
            return Bound{ texture->getImage(0)->getFileName(), SceneUtil::getTextureType(stateSet, *texture, unit) };
        }

        /// **A diffuse map gains its companions at the next free units, typed so a reader knows
        /// them, and addressed and filtered as the diffuse map is.** `_nh` is taken before `_n`,
        /// which is the rasterizer's order; the state set is the loader's, so it is added to in
        /// place.
        TEST(ShaderAutoMapsTest, aDiffuseMapGainsItsCompanionsTypedAndAddressedAsItIs)
        {
            Content content;

            osg::ref_ptr<osg::Group> node = new osg::Group;
            osg::StateSet* const stateSet = node->getOrCreateStateSet();
            osg::ref_ptr<osg::Texture2D> diffuse = named("textures/stone.dds");
            diffuse->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            diffuse->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            stateSet->setTextureAttributeAndModes(0, diffuse);

            MapVisitor visitor(sEverything, content.mImages);
            node->accept(visitor);

            ASSERT_EQ(node->getStateSet(), stateSet) << "a loader's state set is added to in place";
            ASSERT_EQ(stateSet->getTextureAttributeList().size(), 3u);

            const Bound normal = boundAt(*stateSet, 1);
            EXPECT_EQ(normal.mFile, "textures/stone_nh.dds");
            EXPECT_EQ(normal.mType, "normalHeightMap");

            const Bound specular = boundAt(*stateSet, 2);
            EXPECT_EQ(specular.mFile, "textures/stone_spec.dds");
            EXPECT_EQ(specular.mType, "specularMap");

            const auto* added
                = static_cast<const osg::Texture*>(stateSet->getTextureAttribute(1, osg::StateAttribute::TEXTURE));
            EXPECT_EQ(added->getWrap(osg::Texture::WRAP_S), osg::Texture::CLAMP_TO_EDGE);
            EXPECT_EQ(added->getWrap(osg::Texture::WRAP_T), diffuse->getWrap(osg::Texture::WRAP_T));
            EXPECT_EQ(added->getFilter(osg::Texture::MIN_FILTER), osg::Texture::NEAREST);
        }

        /// **Each map is looked for on its own terms**: `_n` where there is no `_nh`, nothing where
        /// neither file exists, nothing of a kind the switches leave out or the state set already
        /// binds, and no normal map that is the bump map's own file. A drawable's state set is
        /// reached as a node's is.
        TEST(ShaderAutoMapsTest, eachMapIsLookedForOnlyWhereTheRuleAndTheStateSetLeaveRoom)
        {
            Content content;

            const auto attached = [&](const AutoMapRules& rules, const std::string& diffuse,
                                      const std::string& boundType = {}, const std::string& boundFile = {}) {
                osg::ref_ptr<osg::Geode> geode = new osg::Geode;
                osg::ref_ptr<osg::Geometry> drawable = new osg::Geometry;
                geode->addDrawable(drawable);
                osg::StateSet* const stateSet = drawable->getOrCreateStateSet();
                stateSet->setTextureAttributeAndModes(0, named(diffuse));
                stateSet->setTextureAttribute(0, new SceneUtil::TextureType("diffuseMap"));
                if (!boundType.empty())
                {
                    stateSet->setTextureAttributeAndModes(1, named(boundFile));
                    stateSet->setTextureAttribute(1, new SceneUtil::TextureType(boundType));
                }

                MapVisitor visitor(rules, content.mImages);
                geode->accept(visitor);

                std::string types;
                for (unsigned int unit = boundType.empty() ? 1 : 2; unit < stateSet->getTextureAttributeList().size();
                     ++unit)
                {
                    const Bound bound = boundAt(*stateSet, unit);
                    types += bound.mType + "=" + bound.mFile + " ";
                }
                return types;
            };

            EXPECT_EQ(attached(sEverything, "textures/wood.dds"), "normalMap=textures/wood_n.dds ");
            EXPECT_EQ(attached(sEverything, "textures/sand.dds"), "");

            AutoMapRules noNormals = sEverything;
            noNormals.mNormalMaps = false;
            EXPECT_EQ(attached(noNormals, "textures/stone.dds"), "specularMap=textures/stone_spec.dds ");

            AutoMapRules nothing = sEverything;
            nothing.mNormalMaps = false;
            nothing.mSpecularMaps = false;
            EXPECT_EQ(attached(nothing, "textures/stone.dds"), "");

            EXPECT_EQ(attached(sEverything, "textures/stone.dds", "normalMap", "textures/own_normal.dds"),
                "specularMap=textures/stone_spec.dds ")
                << "a normal map the state set binds is not replaced";
            EXPECT_EQ(attached(sEverything, "textures/stone.dds", "specularMap", "textures/own_spec.dds"),
                "normalHeightMap=textures/stone_nh.dds ")
                << "a specular map the state set binds is not replaced";
            EXPECT_EQ(attached(sEverything, "textures/stone.dds", "bumpMap", "textures/stone_nh.dds"),
                "specularMap=textures/stone_spec.dds ")
                << "a normal map that is the bump map's own file is not added";
        }

        /// A unit square in the plane of x and y, facing up, its two triangles counter-clockwise.
        osg::ref_ptr<osg::Geometry> makeSquare()
        {
            osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
            positions->push_back(osg::Vec3f(0.0f, 0.0f, 0.0f));
            positions->push_back(osg::Vec3f(1.0f, 0.0f, 0.0f));
            positions->push_back(osg::Vec3f(1.0f, 1.0f, 0.0f));
            positions->push_back(osg::Vec3f(0.0f, 1.0f, 0.0f));

            osg::ref_ptr<osg::DrawElementsUShort> triangles = new osg::DrawElementsUShort(GL_TRIANGLES);
            for (const unsigned short index : { 0, 1, 2, 0, 2, 3 })
                triangles->push_back(index);

            osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
            normals->resize(4, osg::Vec3f(0.0f, 0.0f, 1.0f));

            osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
            geometry->setVertexArray(positions);
            geometry->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
            geometry->addPrimitiveSet(triangles);
            return geometry;
        }

        /// The square's corners as texture coordinates, `u` along x — or against it, `mirrored`,
        /// which is a texture flipped across the face.
        osg::ref_ptr<osg::Vec2Array> squareCoordinates(bool mirrored)
        {
            osg::ref_ptr<osg::Vec2Array> coordinates = new osg::Vec2Array;
            for (const osg::Vec2f corner : { osg::Vec2f(0, 0), osg::Vec2f(1, 0), osg::Vec2f(1, 1), osg::Vec2f(0, 1) })
                coordinates->push_back(mirrored ? osg::Vec2f(1.0f - corner.x(), corner.y()) : corner);
            return coordinates;
        }

        /// What `geometry` carries at the tangents' unit, as text: one tangent per vertex, or
        /// `none`.
        std::string tangentsOf(const osg::Geometry& geometry)
        {
            const auto* tangents = dynamic_cast<const osg::Vec4Array*>(geometry.getTexCoordArray(sTangentUnit));
            if (tangents == nullptr)
                return "none";

            std::string text;
            for (const osg::Vec4f& tangent : *tangents)
                text += "(" + std::to_string(static_cast<int>(tangent.x())) + " "
                    + std::to_string(static_cast<int>(tangent.y())) + " "
                    + std::to_string(static_cast<int>(tangent.z())) + " "
                    + std::to_string(static_cast<int>(tangent.w())) + ")";
            return text;
        }

        /// **A drawable a normal map is read through gains the tangents the rasterizer's shader
        /// visitor would build, and no other drawable gains any.** The square's `u` runs along x
        /// and `v` along y, so its tangent is x at every corner and `cross(N, T) = cross(z, x) = y`
        /// is its bitangent: the handedness is one. Mirrored, `u` runs against x, the tangent is
        /// minus x, and `cross(z, -x) = -y` is against `v`, which is what a handedness of minus one
        /// turns back. Every value is exact, because every sum the generator takes is of whole
        /// numbers.
        ///
        /// The coordinates are the normal map's own unit's where it has some, and unit nought's
        /// where it has none. A normal map is in force below the state set that binds it and not
        /// beside it, and a skinned drawable carries its tangents on the source geometry it is
        /// posed from.
        TEST(ShaderAutoMapsTest, aNormalMapBringsTheTangentsItIsReadThrough)
        {
            Content content;

            const std::string alongX = "(1 0 0 1)(1 0 0 1)(1 0 0 1)(1 0 0 1)";
            const std::string againstX = "(-1 0 0 -1)(-1 0 0 -1)(-1 0 0 -1)(-1 0 0 -1)";

            /// The square under a node whose state set holds `diffuse`, with `normalMap` bound at
            /// unit one where it is named, and coordinates at unit one where `atOne` is not null.
            const auto visited = [&](const AutoMapRules& rules, const std::string& diffuse,
                                     const std::string& normalMap, bool mirrored, osg::ref_ptr<osg::Vec2Array> atOne) {
                osg::ref_ptr<osg::Geode> geode = new osg::Geode;
                osg::StateSet* const stateSet = geode->getOrCreateStateSet();
                stateSet->setTextureAttributeAndModes(0, named(diffuse));
                if (!normalMap.empty())
                {
                    stateSet->setTextureAttributeAndModes(1, named(normalMap));
                    stateSet->setTextureAttribute(1, new SceneUtil::TextureType("normalMap"));
                }

                osg::ref_ptr<osg::Geometry> square = makeSquare();
                square->setTexCoordArray(0, squareCoordinates(mirrored));
                if (atOne != nullptr)
                    square->setTexCoordArray(1, atOne);
                geode->addDrawable(square);

                MapVisitor visitor(rules, content.mImages);
                geode->accept(visitor);
                return tangentsOf(*square);
            };

            EXPECT_EQ(visited(sEverything, "textures/stone.dds", {}, false, nullptr), alongX);
            EXPECT_EQ(visited(sEverything, "textures/stone.dds", {}, true, nullptr), againstX);
            EXPECT_EQ(visited(sEverything, "textures/sand.dds", {}, false, nullptr), "none")
                << "a drawable no normal map is read through gained tangents";

            // A normal map the content binds, found with every rule off, and read through unit
            // one's coordinates, which are mirrored where unit nought's are not.
            AutoMapRules nothing = sEverything;
            nothing.mNormalMaps = false;
            nothing.mSpecularMaps = false;
            EXPECT_EQ(
                visited(nothing, "textures/sand.dds", "textures/sand_n.dds", false, squareCoordinates(true)), againstX);
            EXPECT_EQ(visited(nothing, "textures/sand.dds", "textures/sand_n.dds", false, nullptr), alongX)
                << "unit one has no coordinates, so unit nought's are read";

            // No coordinates at all: nothing to build from, and nothing is built.
            {
                osg::ref_ptr<osg::Geode> geode = new osg::Geode;
                geode->getOrCreateStateSet()->setTextureAttributeAndModes(0, named("textures/stone.dds"));
                osg::ref_ptr<osg::Geometry> square = makeSquare();
                geode->addDrawable(square);
                MapVisitor visitor(sEverything, content.mImages);
                geode->accept(visitor);
                EXPECT_EQ(tangentsOf(*square), "none") << "tangents from no coordinates";
            }

            // Two siblings: the first's normal map is not the second's.
            {
                osg::ref_ptr<osg::Group> group = new osg::Group;
                osg::ref_ptr<osg::Geode> mapped = new osg::Geode;
                mapped->getOrCreateStateSet()->setTextureAttributeAndModes(0, named("textures/stone.dds"));
                osg::ref_ptr<osg::Geometry> first = makeSquare();
                first->setTexCoordArray(0, squareCoordinates(false));
                mapped->addDrawable(first);
                osg::ref_ptr<osg::Geode> plain = new osg::Geode;
                osg::ref_ptr<osg::Geometry> second = makeSquare();
                second->setTexCoordArray(0, squareCoordinates(false));
                plain->addDrawable(second);
                group->addChild(mapped);
                group->addChild(plain);

                MapVisitor visitor(sEverything, content.mImages);
                group->accept(visitor);
                EXPECT_EQ(tangentsOf(*first), alongX);
                EXPECT_EQ(tangentsOf(*second), "none") << "a normal map reached past its own subtree";
            }

            // A skinned square: the tangents on the source geometry it is posed from.
            {
                osg::ref_ptr<osg::Geode> geode = new osg::Geode;
                geode->getOrCreateStateSet()->setTextureAttributeAndModes(0, named("textures/stone.dds"));
                osg::ref_ptr<osg::Geometry> source = makeSquare();
                source->setTexCoordArray(0, squareCoordinates(true));
                osg::ref_ptr<SceneUtil::RigGeometry> rig = new SceneUtil::RigGeometry;
                rig->setSourceGeometry(source);
                geode->addDrawable(rig);

                MapVisitor visitor(sEverything, content.mImages);
                geode->accept(visitor);
                EXPECT_EQ(tangentsOf(*rig->getSourceGeometry()), againstX);
            }
        }
    }
}

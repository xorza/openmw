#include "../nif/node.hpp"

#include <components/nif/data.hpp>
#include <components/nif/node.hpp>
#include <components/nif/property.hpp>
#include <components/nifosg/controller.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/serialize.hpp>
#include <components/surface/material.hpp>
#include <components/vfs/manager.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <osgDB/Registry>

#include <array>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    using namespace testing;
    using namespace NifOsg;
    using namespace Nif::Testing;

    constexpr VFS::Path::NormalizedView testNif("test.nif");

    struct BaseNifOsgLoaderTest
    {
        VFS::Manager mVfs;
        Resource::ImageManager mImageManager{ &mVfs, 0 };
        Resource::BgsmFileManager mMaterialManager{ &mVfs, 0 };
        const osgDB::ReaderWriter* mReaderWriter = osgDB::Registry::instance()->getReaderWriterForExtension("osgt");
        osg::ref_ptr<osgDB::Options> mOptions = new osgDB::Options;

        BaseNifOsgLoaderTest()
        {
            // The loader asks its host what a hidden node carries before it reads anything, and a
            // test that calls `Loader::load` directly is a host. Nothing here hides a node, so the
            // answer is the one a caller that does not care gives.
            Loader::configure({});

            SceneUtil::registerSerializers();

            if (mReaderWriter == nullptr)
                throw std::runtime_error("osgt reader writer is not found");

            mOptions->setPluginStringData("fileType", "Ascii");
            mOptions->setPluginStringData("WriteImageHint", "UseExternal");
        }

        std::string serialize(const osg::Node& node) const
        {
            std::stringstream stream;
            mReaderWriter->writeNode(node, stream, mOptions);
            std::string result;
            for (std::string line; std::getline(stream, line);)
            {
                if (line.starts_with('#'))
                    continue;
                line.erase(line.find_last_not_of(" \t\n\r\f\v") + 1);
                result += line;
                result += '\n';
            }
            return result;
        }
    };

    /// Finds the surface description the loader authored, wherever in the graph it landed.
    struct FindMaterial : osg::NodeVisitor
    {
        const Surface::Material* mFound = nullptr;

        FindMaterial()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
            setNodeMaskOverride(~0u);
        }

        void apply(osg::Node& node) override
        {
            if (node.getStateSet() != nullptr)
                if (const Surface::Material* material = Surface::getMaterial(*node.getStateSet()))
                    mFound = material;

            traverse(node);
        }
    };

    struct NifOsgLoaderTest : Test, BaseNifOsgLoaderTest
    {
        /// The description the loader authors for one triangle carrying `properties`, or nothing
        /// where it authored none.
        ///
        /// A copy and not a pointer, because the graph the description hangs on dies with the call.
        std::optional<Surface::Material> describeTriangle(std::initializer_list<Nif::NiProperty*> properties)
        {
            Nif::NiTriShapeData data;
            data.mRecordType = Nif::RC_NiTriShapeData;
            data.mVertices = { osg::Vec3f(0, 0, 0), osg::Vec3f(1, 0, 0), osg::Vec3f(1, 1, 0) };
            data.mNumVertices = 3;
            data.mTriangles = { 0, 1, 2 };

            Nif::NiTriShape shape;
            init(shape);
            shape.mData = Nif::NiGeometryDataPtr(&data);
            for (Nif::NiProperty* property : properties)
                shape.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(property));

            Nif::NIFFile file(testNif);
            file.mRoots.push_back(&shape);
            osg::ref_ptr<osg::Node> result = Loader::load(file, &mImageManager, &mMaterialManager);

            FindMaterial find;
            result->accept(find);
            if (find.mFound == nullptr)
                return std::nullopt;
            return *find.mFound;
        }
    };

    /// A shape carries what its properties said, and not only what they compiled to.
    ///
    /// **This is the round trip that used to be one.** `NiAlphaProperty` became an `osg::BlendFunc`
    /// and an `osg::AlphaFunc`, `NiStencilProperty` became a `GL_CULL_FACE` mode, `NiMaterialProperty`
    /// became an `osg::Material` — and anything that is not the OpenGL renderer had to read those
    /// back and work out what the content had said. The description is the content's own answer.
    ///
    /// Textures are the sweep's job (`apps/components_tests/rtxtool/material.cpp`): binding one here
    /// would need a VFS with an image in it, and real content exercises every role rather than two.
    TEST_F(NifOsgLoaderTest, shouldDescribeASurfaceFromItsProperties)
    {
        Nif::NiMaterialProperty colours;
        init(static_cast<Nif::NiObjectNET&>(colours));
        colours.mRecordType = Nif::RC_NiMaterialProperty;
        colours.mDiffuse = osg::Vec3f(0.25f, 0.5f, 0.75f);
        colours.mAmbient = osg::Vec3f(0.1f, 0.2f, 0.3f);
        colours.mEmissive = osg::Vec3f(0.5f, 0.25f, 0.0f);
        colours.mAlpha = 0.5f;
        colours.mEmissiveMult = 2.0f;

        Nif::NiAlphaProperty alpha;
        init(static_cast<Nif::NiObjectNET&>(alpha));
        alpha.mRecordType = Nif::RC_NiAlphaProperty;
        alpha.mFlags = Nif::NiAlphaProperty::Flag_Testing;
        alpha.mThreshold = 128;

        Nif::NiStencilProperty stencil;
        init(static_cast<Nif::NiObjectNET&>(stencil));
        stencil.mRecordType = Nif::RC_NiStencilProperty;
        stencil.mDrawMode = Nif::NiStencilProperty::DrawMode::Both;
        stencil.mTestFunction = Nif::NiStencilProperty::TestFunc::Always;
        stencil.mFailAction = Nif::NiStencilProperty::Action::Keep;
        stencil.mZFailAction = Nif::NiStencilProperty::Action::Keep;
        stencil.mPassAction = Nif::NiStencilProperty::Action::Keep;

        const std::optional<Surface::Material> found = describeTriangle({ &colours, &alpha, &stencil });
        ASSERT_TRUE(found.has_value()) << "every shape the loader builds is described";

        // Alpha testing and no blending, so the surface is a cutout at the threshold over 255.
        EXPECT_EQ(found->mAlphaMode, Surface::AlphaMode::Cutout);
        EXPECT_FLOAT_EQ(found->mAlphaRef, 128.0f / 255.0f);

        // Two-sided, which is what `DrawMode::Both` asks for and nothing else in a NIF does: the
        // scene root culls back faces, so a surface nothing spoke about shows one. The test below
        // is the other half.
        EXPECT_TRUE(found->mTwoSided);

        // The material's alpha rides in the diffuse colour, which is where the NIF keeps it.
        EXPECT_EQ(found->mDiffuseColour, osg::Vec4f(0.25f, 0.5f, 0.75f, 0.5f));
        EXPECT_EQ(found->mAmbientColour, osg::Vec3f(0.1f, 0.2f, 0.3f));
        EXPECT_EQ(found->mEmissiveColour, osg::Vec3f(0.5f, 0.25f, 0.0f));
        EXPECT_FLOAT_EQ(found->mEmissiveMult, 2.0f);

        // Morrowind has specular lighting off, and the loader zeroes it rather than describing what
        // the record happens to hold.
        EXPECT_EQ(found->mSpecularColour, osg::Vec3f(0.0f, 0.0f, 0.0f));
        EXPECT_FLOAT_EQ(found->mGlossiness, 0.0f);
    }

    /// A surface shows one face unless a stencil property draws both.
    ///
    /// **The default is what the scene root does, and the record is the only thing that changes
    /// it.** The game turns `GL_CULL_FACE` on over the whole scene and the three shipped archives
    /// hold no `NiStencilProperty` at all, so nearly every surface in Morrowind is the first row.
    /// The three shapes differ in the one property, so the difference in the answer can only be the
    /// draw mode — and the two-sided row is what keeps the other two from being an assertion of the
    /// default.
    TEST_F(NifOsgLoaderTest, aSurfaceShowsOneFaceUnlessAStencilPropertyDrawsBoth)
    {
        using DrawMode = Nif::NiStencilProperty::DrawMode;

        const auto describedWith = [this](std::optional<DrawMode> drawMode) {
            Nif::NiStencilProperty stencil;
            init(static_cast<Nif::NiObjectNET&>(stencil));
            stencil.mRecordType = Nif::RC_NiStencilProperty;
            stencil.mDrawMode = drawMode.value_or(DrawMode::Default);
            stencil.mTestFunction = Nif::NiStencilProperty::TestFunc::Always;
            stencil.mFailAction = Nif::NiStencilProperty::Action::Keep;
            stencil.mZFailAction = Nif::NiStencilProperty::Action::Keep;
            stencil.mPassAction = Nif::NiStencilProperty::Action::Keep;

            const std::optional<Surface::Material> found
                = drawMode.has_value() ? describeTriangle({ &stencil }) : describeTriangle({});
            EXPECT_TRUE(found.has_value());
            return found.has_value() && found->mTwoSided;
        };

        EXPECT_FALSE(describedWith(std::nullopt)) << "nothing spoke, and the scene root culls";
        EXPECT_FALSE(describedWith(DrawMode::CounterClockwise));
        EXPECT_TRUE(describedWith(DrawMode::Both));
    }

    /// A source that always says the same thing, so a controller's output is what it computed and
    /// not what a clock happened to read.
    struct FixedSource : SceneUtil::ControllerSource
    {
        float mValue = 0.0f;

        float getValue(osg::NodeVisitor*) override { return mValue; }
    };

    /// One key held for all time, which is what an interpolator needs to answer at all.
    Nif::FloatKeyMapPtr constantKey(float value)
    {
        auto keys = std::make_shared<Nif::FloatKeyMap>();
        keys->mInterpolationType = Nif::InterpolationType_Linear;
        keys->mKeys.emplace_back(0.0f, Nif::FloatKeyMap::KeyType{ value, 0.0f, 0.0f });
        return keys;
    }

    /// A scrolling surface says so in its description, not only in the matrix it hands OpenGL.
    ///
    /// **This is the fact a ray tracer could not see.** `UVController` wrote an `osg::TexMat` and
    /// nothing else, so a texture animated by scrolling its UVs stood still in anything that samples
    /// a texture rather than binding one — and there was nowhere in a material to put it, because
    /// there was no material. The scale and the offset are the two numbers the matrix is built from.
    TEST_F(NifOsgLoaderTest, aScrollingSurfaceDescribesTheTransformItAnimates)
    {
        Nif::NiUVData data;
        data.mKeyList[0] = constantKey(0.25f); // U offset, which the convention negates
        data.mKeyList[1] = constantKey(0.5f); // V offset, which it does not
        data.mKeyList[2] = constantKey(2.0f); // U scale
        data.mKeyList[3] = constantKey(4.0f); // V scale

        osg::ref_ptr<UVController> controller = new UVController(&data, { 0u });
        auto source = std::make_shared<FixedSource>();
        controller->setSource(source);

        osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
        Surface::setMaterial(*state, Surface::Material{});
        controller->setDefaults(state);
        controller->apply(state, nullptr);

        const Surface::Material* described = Surface::getMaterial(*state);
        ASSERT_NE(described, nullptr);
        EXPECT_EQ(described->mTextureScale, osg::Vec2f(2.0f, 4.0f));
        EXPECT_EQ(described->mTextureOffset, osg::Vec2f(-0.25f, 0.5f));
    }

    /// Blending wins over testing, and the threshold survives for a renderer that would rather cut.
    TEST_F(NifOsgLoaderTest, shouldDescribeABlendedSurfaceAsBlendedAndKeepItsThreshold)
    {
        Nif::NiAlphaProperty alpha;
        init(static_cast<Nif::NiObjectNET&>(alpha));
        alpha.mRecordType = Nif::RC_NiAlphaProperty;
        alpha.mFlags = Nif::NiAlphaProperty::Flag_Blending | Nif::NiAlphaProperty::Flag_Testing;
        alpha.mThreshold = 64;

        const std::optional<Surface::Material> found = describeTriangle({ &alpha });
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->mAlphaMode, Surface::AlphaMode::Blend);
        EXPECT_FLOAT_EQ(found->mAlphaRef, 64.0f / 255.0f);
    }

    TEST_F(NifOsgLoaderTest, shouldLoadFileWithDefaultNode)
    {
        Nif::NiAVObject node;
        init(node);
        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&node);
        auto result = Loader::load(file, &mImageManager, &mMaterialManager);
        EXPECT_EQ(serialize(*result), R"(
osg::Group {
  UniqueID 1
  DataVariance STATIC
  UserDataContainer TRUE {
    osg::DefaultUserDataContainer {
      UniqueID 2
      UDC_UserObjects 1 {
        osg::StringValueObject {
          UniqueID 3
          Name "fileHash"
        }
      }
    }
  }
  Children 1 {
    osg::Group {
      UniqueID 4
      DataVariance STATIC
      UserDataContainer TRUE {
        osg::DefaultUserDataContainer {
          UniqueID 5
          UDC_UserObjects 1 {
            osg::UIntValueObject {
              UniqueID 6
              Name "recordIndex"
              Value 4294967295
            }
          }
        }
      }
    }
  }
}
)");
    }

    std::string formatOsgNodeForBSShaderProperty(std::string_view shaderPrefix)
    {
        std::ostringstream oss;
        oss << R"(
osg::Group {
  UniqueID 1
  DataVariance STATIC
  UserDataContainer TRUE {
    osg::DefaultUserDataContainer {
      UniqueID 2
      UDC_UserObjects 1 {
        osg::StringValueObject {
          UniqueID 3
          Name "fileHash"
        }
      }
    }
  }
  Children 1 {
    osg::Group {
      UniqueID 4
      DataVariance STATIC
      UserDataContainer TRUE {
        osg::DefaultUserDataContainer {
          UniqueID 5
          UDC_UserObjects 2 {
            osg::UIntValueObject {
              UniqueID 6
              Name "recordIndex"
              Value 4294967295
            }
            osg::StringValueObject {
              UniqueID 7
              Name "shaderPrefix"
              Value ")"
            << shaderPrefix << R"("
            }
          }
        }
      }
      StateSet TRUE {
        osg::StateSet {
          UniqueID 8
        }
      }
    }
  }
}
)";
        return oss.str();
    }

    std::string formatOsgNodeForBSLightingShaderProperty(std::string_view shaderPrefix)
    {
        std::ostringstream oss;
        oss << R"(
osg::Group {
  UniqueID 1
  DataVariance STATIC
  UserDataContainer TRUE {
    osg::DefaultUserDataContainer {
      UniqueID 2
      UDC_UserObjects 1 {
        osg::StringValueObject {
          UniqueID 3
          Name "fileHash"
        }
      }
    }
  }
  Children 1 {
    osg::Group {
      UniqueID 4
      DataVariance STATIC
      UserDataContainer TRUE {
        osg::DefaultUserDataContainer {
          UniqueID 5
          UDC_UserObjects 2 {
            osg::UIntValueObject {
              UniqueID 6
              Name "recordIndex"
              Value 4294967295
            }
            osg::StringValueObject {
              UniqueID 7
              Name "shaderPrefix"
              Value ")"
            << shaderPrefix << R"("
            }
          }
        }
      }
      StateSet TRUE {
        osg::StateSet {
          UniqueID 8
          ModeList 1 {
            GL_DEPTH_TEST ON
          }
          AttributeList 1 {
            osg::Depth {
              UniqueID 9
              Function LEQUAL
            }
            Value OFF
          }
        }
      }
    }
  }
}
)";
        return oss.str();
    }

    struct ShaderPrefixParams
    {
        unsigned int mShaderType;
        std::string_view mExpectedShaderPrefix;
    };

    struct NifOsgLoaderBSShaderPrefixTest : TestWithParam<ShaderPrefixParams>, BaseNifOsgLoaderTest
    {
        static constexpr std::array sParams = {
            ShaderPrefixParams{ static_cast<unsigned int>(Nif::BSShaderType::ShaderType_Default), "bs/default" },
            ShaderPrefixParams{ static_cast<unsigned int>(Nif::BSShaderType::ShaderType_NoLighting), "bs/nolighting" },
            ShaderPrefixParams{ static_cast<unsigned int>(Nif::BSShaderType::ShaderType_Tile), "bs/default" },
            ShaderPrefixParams{ std::numeric_limits<unsigned int>::max(), "bs/default" },
        };
    };

    TEST_P(NifOsgLoaderBSShaderPrefixTest, shouldAddShaderPrefix)
    {
        Nif::NiAVObject node;
        init(node);
        Nif::BSShaderPPLightingProperty property;
        property.mRecordType = Nif::RC_BSShaderPPLightingProperty;
        property.mTextureSet = nullptr;
        property.mController = nullptr;
        property.mType = GetParam().mShaderType;
        node.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&property));
        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&node);
        auto result = Loader::load(file, &mImageManager, &mMaterialManager);
        EXPECT_EQ(serialize(*result), formatOsgNodeForBSShaderProperty(GetParam().mExpectedShaderPrefix));
    }

    INSTANTIATE_TEST_SUITE_P(Params, NifOsgLoaderBSShaderPrefixTest, ValuesIn(NifOsgLoaderBSShaderPrefixTest::sParams));

    struct NifOsgLoaderBSLightingShaderPrefixTest : TestWithParam<ShaderPrefixParams>, BaseNifOsgLoaderTest
    {
        static constexpr std::array sParams = {
            ShaderPrefixParams{
                static_cast<unsigned int>(Nif::BSLightingShaderType::ShaderType_Default), "bs/default" },
            ShaderPrefixParams{ static_cast<unsigned int>(Nif::BSLightingShaderType::ShaderType_Cloud), "bs/default" },
            ShaderPrefixParams{ std::numeric_limits<unsigned int>::max(), "bs/default" },
        };
    };

    TEST_P(NifOsgLoaderBSLightingShaderPrefixTest, shouldAddShaderPrefix)
    {
        Nif::NiAVObject node;
        init(node);
        Nif::BSLightingShaderProperty property;
        property.mRecordType = Nif::RC_BSLightingShaderProperty;
        property.mTextureSet = nullptr;
        property.mController = nullptr;
        property.mType = GetParam().mShaderType;
        property.mShaderFlags1 |= Nif::BSShaderFlags1::BSSFlag1_DepthTest;
        property.mShaderFlags2 |= Nif::BSShaderFlags2::BSSFlag2_DepthWrite;
        node.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&property));
        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&node);
        auto result = Loader::load(file, &mImageManager, &mMaterialManager);
        EXPECT_EQ(serialize(*result), formatOsgNodeForBSLightingShaderProperty(GetParam().mExpectedShaderPrefix));
    }

    INSTANTIATE_TEST_SUITE_P(
        Params, NifOsgLoaderBSLightingShaderPrefixTest, ValuesIn(NifOsgLoaderBSLightingShaderPrefixTest::sParams));
}

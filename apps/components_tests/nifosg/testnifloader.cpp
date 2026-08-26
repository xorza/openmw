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
#include <limits>
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

    struct NifOsgLoaderTest : Test, BaseNifOsgLoaderTest
    {
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
        Nif::NiTriShapeData data;
        data.mRecordType = Nif::RC_NiTriShapeData;
        data.mVertices = { osg::Vec3f(0, 0, 0), osg::Vec3f(1, 0, 0), osg::Vec3f(1, 1, 0) };
        data.mNumVertices = 3;
        data.mTriangles = { 0, 1, 2 };

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

        Nif::NiTriShape shape;
        init(shape);
        shape.mData = Nif::NiGeometryDataPtr(&data);
        shape.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&colours));
        shape.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&alpha));
        shape.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&stencil));

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&shape);
        osg::ref_ptr<osg::Node> result = Loader::load(file, &mImageManager, &mMaterialManager);

        FindMaterial find;
        result->accept(find);
        ASSERT_NE(find.mFound, nullptr) << "every shape the loader builds is described";

        // Alpha testing and no blending, so the surface is a cutout at the threshold over 255.
        EXPECT_EQ(find.mFound->mAlphaMode, Surface::AlphaMode::Cutout);
        EXPECT_FLOAT_EQ(find.mFound->mAlphaRef, 128.0f / 255.0f);

        // Two-sided, which is what `DrawMode::Both` asks for — and, on its own, also what a surface
        // nothing spoke about would have said. The test below is what makes this one mean something.
        EXPECT_TRUE(find.mFound->mTwoSided);

        // The material's alpha rides in the diffuse colour, which is where the NIF keeps it.
        EXPECT_EQ(find.mFound->mDiffuseColour, osg::Vec4f(0.25f, 0.5f, 0.75f, 0.5f));
        EXPECT_EQ(find.mFound->mAmbientColour, osg::Vec3f(0.1f, 0.2f, 0.3f));
        EXPECT_EQ(find.mFound->mEmissiveColour, osg::Vec3f(0.5f, 0.25f, 0.0f));
        EXPECT_FLOAT_EQ(find.mFound->mEmissiveMult, 2.0f);

        // Morrowind has specular lighting off, and the loader zeroes it rather than describing what
        // the record happens to hold.
        EXPECT_EQ(find.mFound->mSpecularColour, osg::Vec3f(0.0f, 0.0f, 0.0f));
        EXPECT_FLOAT_EQ(find.mFound->mGlossiness, 0.0f);
    }

    /// A stencil property is the one record that makes a surface single-sided.
    ///
    /// **The direction that has to be tested, because the other one is the default.** OpenGL culls
    /// nothing until told to and no other NIF record turns culling on, so `mTwoSided` is true for
    /// almost everything and an assertion that it is true proves nothing on its own. This is the
    /// same shape as the test above with the draw mode changed, so the difference in the answer can
    /// only be the draw mode.
    TEST_F(NifOsgLoaderTest, aStencilPropertyThatDrawsOneFaceDescribesASingleSidedSurface)
    {
        Nif::NiTriShapeData data;
        data.mRecordType = Nif::RC_NiTriShapeData;
        data.mVertices = { osg::Vec3f(0, 0, 0), osg::Vec3f(1, 0, 0), osg::Vec3f(1, 1, 0) };
        data.mNumVertices = 3;
        data.mTriangles = { 0, 1, 2 };

        Nif::NiStencilProperty stencil;
        init(static_cast<Nif::NiObjectNET&>(stencil));
        stencil.mRecordType = Nif::RC_NiStencilProperty;
        stencil.mDrawMode = Nif::NiStencilProperty::DrawMode::CounterClockwise;
        stencil.mTestFunction = Nif::NiStencilProperty::TestFunc::Always;
        stencil.mFailAction = Nif::NiStencilProperty::Action::Keep;
        stencil.mZFailAction = Nif::NiStencilProperty::Action::Keep;
        stencil.mPassAction = Nif::NiStencilProperty::Action::Keep;

        Nif::NiTriShape shape;
        init(shape);
        shape.mData = Nif::NiGeometryDataPtr(&data);
        shape.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&stencil));

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&shape);
        osg::ref_ptr<osg::Node> result = Loader::load(file, &mImageManager, &mMaterialManager);

        FindMaterial find;
        result->accept(find);
        ASSERT_NE(find.mFound, nullptr);

        EXPECT_FALSE(find.mFound->mTwoSided);
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
        Nif::NiTriShapeData data;
        data.mRecordType = Nif::RC_NiTriShapeData;
        data.mVertices = { osg::Vec3f(0, 0, 0), osg::Vec3f(1, 0, 0), osg::Vec3f(1, 1, 0) };
        data.mNumVertices = 3;
        data.mTriangles = { 0, 1, 2 };

        Nif::NiAlphaProperty alpha;
        init(static_cast<Nif::NiObjectNET&>(alpha));
        alpha.mRecordType = Nif::RC_NiAlphaProperty;
        alpha.mFlags = Nif::NiAlphaProperty::Flag_Blending | Nif::NiAlphaProperty::Flag_Testing;
        alpha.mThreshold = 64;

        Nif::NiTriShape shape;
        init(shape);
        shape.mData = Nif::NiGeometryDataPtr(&data);
        shape.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(&alpha));

        Nif::NIFFile file(testNif);
        file.mRoots.push_back(&shape);
        osg::ref_ptr<osg::Node> result = Loader::load(file, &mImageManager, &mMaterialManager);

        FindMaterial find;
        result->accept(find);
        ASSERT_NE(find.mFound, nullptr);
        EXPECT_EQ(find.mFound->mAlphaMode, Surface::AlphaMode::Blend);
        EXPECT_FLOAT_EQ(find.mFound->mAlphaRef, 64.0f / 255.0f);
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

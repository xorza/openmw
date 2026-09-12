#include "../nif/node.hpp"

#include <osg/Drawable>
#include <osg/StateSet>

#include <components/nif/data.hpp>
#include <components/nif/node.hpp>
#include <components/nif/property.hpp>
#include <components/nifosg/controller.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/rtx/surface.hpp>
#include <components/vfs/manager.hpp>

#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <vector>

namespace Rtx
{
    namespace
    {
        using namespace testing;
        using namespace NifOsg;
        using namespace Nif::Testing;

        constexpr VFS::Path::NormalizedView testNif("test.nif");

        /// Folds the state sets in force at the first drawable, root first, the way a walk does.
        struct FindMaterial : osg::NodeVisitor
        {
            std::optional<Rtx::SurfaceDescription> mFound;
            std::vector<const osg::StateSet*> mChain;

            FindMaterial()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
                setNodeMaskOverride(~0u);
            }

            void apply(osg::Node& node) override
            {
                if (node.getStateSet() != nullptr)
                    mChain.push_back(node.getStateSet());

                traverse(node);

                if (node.getStateSet() != nullptr)
                    mChain.pop_back();
            }

            void apply(osg::Drawable& drawable) override
            {
                if (mFound.has_value())
                    return;

                Rtx::SurfaceDescription material;
                bool said = false;
                for (const osg::StateSet* state : mChain)
                    said = Rtx::describeStateSet(*state, material) || said;
                if (drawable.getStateSet() != nullptr)
                    said = Rtx::describeStateSet(*drawable.getStateSet(), material) || said;

                if (said)
                    mFound = material;
            }
        };

        /// What the loader builds in OSG's terms, read back in `Surface`'s.
        struct RtxNifSurfaceTest : Test
        {
            VFS::Manager mVfs;
            Resource::ImageManager mImageManager{ &mVfs, 0 };
            Resource::BgsmFileManager mMaterialManager{ &mVfs, 0 };

            /// What one triangle carrying `properties` describes as, read off the state the loader
            /// built, or nothing where it built no material and no texture.
            std::optional<Rtx::SurfaceDescription> describeTriangle(std::initializer_list<Nif::NiProperty*> properties)
            {
                Nif::NiTriShapeData data;
                data.mRecordType = Nif::RC_NiTriShapeData;
                data.mVertices = { osg::Vec3f(0, 0, 0), osg::Vec3f(1, 0, 0), osg::Vec3f(1, 1, 0) };
                data.mNumVertices = 3;
                data.mTriangles = { 0, 1, 2 };

                Nif::NiTriShape shape;
                init(shape);
                // `init` leaves the two pointers `NiGeometry` reads in `load` unset, and a set pointer
                // is what its `empty()` asserts on.
                shape.mShaderProperty = Nif::BSShaderPropertyPtr(nullptr);
                shape.mAlphaProperty = Nif::NiAlphaPropertyPtr(nullptr);
                shape.mData = Nif::NiGeometryDataPtr(&data);
                for (Nif::NiProperty* property : properties)
                    shape.mProperties.push_back(Nif::RecordPtrT<Nif::NiProperty>(property));

                Nif::NIFFile file(testNif);
                file.mRoots.push_back(&shape);
                osg::ref_ptr<osg::Node> result = Loader::load(file, &mImageManager, &mMaterialManager);

                FindMaterial find;
                result->accept(find);
                return find.mFound;
            }
        };

        /// A shape carries what its properties said, and not only what they compiled to.
        ///
        /// **The round trip, asserted.** `NiAlphaProperty` became an `osg::BlendFunc` and an
        /// `osg::AlphaFunc`, `NiStencilProperty` a `GL_CULL_FACE` mode, `NiMaterialProperty` an
        /// `osg::Material` — and `describeStateSet` reads what the content said back off those.
        ///
        /// Textures are left out: binding one here would need a VFS with an image in it, and what a
        /// role is worth is settled by `RtxSurfaceTest` rather than by a second spelling of it.
        TEST_F(RtxNifSurfaceTest, shouldDescribeASurfaceFromItsProperties)
        {
            Nif::NiMaterialProperty colours;
            init(static_cast<Nif::NiObjectNET&>(colours));
            colours.mRecordType = Nif::RC_NiMaterialProperty;
            colours.mDiffuse = osg::Vec3f(0.25f, 0.5f, 0.75f);
            colours.mAmbient = osg::Vec3f(0.1f, 0.2f, 0.3f);
            colours.mEmissive = osg::Vec3f(0.5f, 0.25f, 0.0f);
            colours.mAlpha = 0.5f;
            colours.mEmissiveMult = 2.0f;

            // Testing at `GL_GREATER`, which is bits ten to twelve of the flags: a test at `GL_ALWAYS`,
            // which is what the flags spell with those bits clear, discards nothing and is no cutout.
            Nif::NiAlphaProperty alpha;
            init(static_cast<Nif::NiObjectNET&>(alpha));
            alpha.mRecordType = Nif::RC_NiAlphaProperty;
            alpha.mFlags = Nif::NiAlphaProperty::Flag_Testing | (4 << 10);
            alpha.mThreshold = 128;

            Nif::NiStencilProperty stencil;
            init(static_cast<Nif::NiObjectNET&>(stencil));
            stencil.mRecordType = Nif::RC_NiStencilProperty;
            stencil.mDrawMode = Nif::NiStencilProperty::DrawMode::Both;
            stencil.mTestFunction = Nif::NiStencilProperty::TestFunc::Always;
            stencil.mFailAction = Nif::NiStencilProperty::Action::Keep;
            stencil.mZFailAction = Nif::NiStencilProperty::Action::Keep;
            stencil.mPassAction = Nif::NiStencilProperty::Action::Keep;

            const std::optional<Rtx::SurfaceDescription> found = describeTriangle({ &colours, &alpha, &stencil });
            ASSERT_TRUE(found.has_value()) << "a shape with a material is described";

            // Alpha testing and no blending, so the surface is a cutout at the threshold over 255.
            EXPECT_EQ(found->mAlphaMode, Rtx::AlphaMode::Cutout);
            EXPECT_FLOAT_EQ(found->mAlphaRef, 128.0f / 255.0f);

            // Two-sided, which is what `DrawMode::Both` asks for and nothing else in a NIF does: the
            // scene root culls back faces, so a surface nothing spoke about shows one. The test below
            // is the other half.
            EXPECT_TRUE(found->mTwoSided);

            // The colours as the record states them, display-encoded, and the alpha beside them rather
            // than inside the diffuse — which is how `NiMaterialProperty` keeps the two.
            EXPECT_EQ(found->mDiffuseColour, (Rtx::EncodedColour{ 0.25f, 0.5f, 0.75f }));
            EXPECT_FLOAT_EQ(found->mOpacity, 0.5f);
            EXPECT_EQ(found->mAmbientColour, (Rtx::EncodedColour{ 0.1f, 0.2f, 0.3f }));
            EXPECT_EQ(found->mEmissiveColour, (Rtx::EncodedColour{ 0.5f, 0.25f, 0.0f }));
            EXPECT_FLOAT_EQ(found->mEmissiveMult, 2.0f);

            // Morrowind has specular lighting off, and the loader zeroes it rather than describing what
            // the record happens to hold.
            EXPECT_EQ(found->mSpecularColour, Rtx::EncodedColour{});
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
        TEST_F(RtxNifSurfaceTest, aSurfaceShowsOneFaceUnlessAStencilPropertyDrawsBoth)
        {
            using DrawMode = Nif::NiStencilProperty::DrawMode;

            const auto describedWith = [this](std::optional<DrawMode> drawMode) {
                // A material, so that the shape is a surface at all: a shape with nothing but a
                // stencil property has set a mode and described nothing.
                Nif::NiMaterialProperty colours;
                init(static_cast<Nif::NiObjectNET&>(colours));
                colours.mRecordType = Nif::RC_NiMaterialProperty;
                colours.mDiffuse = osg::Vec3f(0.5f, 0.5f, 0.5f);

                Nif::NiStencilProperty stencil;
                init(static_cast<Nif::NiObjectNET&>(stencil));
                stencil.mRecordType = Nif::RC_NiStencilProperty;
                stencil.mDrawMode = drawMode.value_or(DrawMode::Default);
                stencil.mTestFunction = Nif::NiStencilProperty::TestFunc::Always;
                stencil.mFailAction = Nif::NiStencilProperty::Action::Keep;
                stencil.mZFailAction = Nif::NiStencilProperty::Action::Keep;
                stencil.mPassAction = Nif::NiStencilProperty::Action::Keep;

                const std::optional<Rtx::SurfaceDescription> found
                    = drawMode.has_value() ? describeTriangle({ &colours, &stencil }) : describeTriangle({ &colours });
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

        /// A scrolling surface's description follows the matrix the controller hands OpenGL.
        ///
        /// **The two numbers come back out of the matrix.** `UVController` writes a `texMat` uniform
        /// built from a scale about the middle of the texture and an offset, and a renderer that
        /// samples a texture rather than binding one wants those two numbers rather than the matrix.
        TEST_F(RtxNifSurfaceTest, aScrollingSurfaceDescribesTheTransformItAnimates)
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
            controller->setDefaults(state);
            controller->apply(state, nullptr);

            Rtx::SurfaceDescription described;
            Rtx::describeStateSet(*state, described);
            EXPECT_EQ(described.mTextureScale, osg::Vec2f(2.0f, 4.0f));
            EXPECT_EQ(described.mTextureOffset, osg::Vec2f(-0.25f, 0.5f));
        }

        /// Blending wins over testing, and the threshold survives for a renderer that would rather cut.
        TEST_F(RtxNifSurfaceTest, shouldDescribeABlendedSurfaceAsBlendedAndKeepItsThreshold)
        {
            Nif::NiAlphaProperty alpha;
            init(static_cast<Nif::NiObjectNET&>(alpha));
            alpha.mRecordType = Nif::RC_NiAlphaProperty;
            alpha.mFlags = Nif::NiAlphaProperty::Flag_Blending | Nif::NiAlphaProperty::Flag_Testing | (4 << 10);
            alpha.mThreshold = 64;

            // A material as well, so that the shape is a surface: an alpha property alone is a mode.
            Nif::NiMaterialProperty colours;
            init(static_cast<Nif::NiObjectNET&>(colours));
            colours.mRecordType = Nif::RC_NiMaterialProperty;
            colours.mDiffuse = osg::Vec3f(0.5f, 0.5f, 0.5f);

            const std::optional<Rtx::SurfaceDescription> found = describeTriangle({ &colours, &alpha });
            ASSERT_TRUE(found.has_value());
            EXPECT_EQ(found->mAlphaMode, Rtx::AlphaMode::Blend);
            EXPECT_FLOAT_EQ(found->mAlphaRef, 64.0f / 255.0f);
        }
    }
}

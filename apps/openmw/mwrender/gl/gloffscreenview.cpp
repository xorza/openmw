#include "gloffscreenview.hpp"

#include <algorithm>

#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Group>
#include <osg/Image>
#include <osg/Material>
#include <osg/PolygonMode>
#include <osg/Texture2D>
#include <osg/Viewport>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>

#include <components/debug/debuglog.hpp>
#include <components/myguiplatform/myguitexture.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/fog.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/values.hpp>
#include <components/stereo/multiview.hpp>

#include "../util.hpp"
#include "../vismask.hpp"

namespace MWRender
{

    /// Updates the subtree, lets it be drawn once, and then takes the whole camera out of the frame
    /// until something asks for the picture again.
    class OffscreenDrawOnceCallback : public SceneUtil::NodeCallback<OffscreenDrawOnceCallback>
    {
    public:
        /// @param subgraph what to bring up to date before the draw, or null where the subtree is
        ///        the world's and the frame's own update traversal has already been through it.
        explicit OffscreenDrawOnceCallback(osg::Node* subgraph)
            : mSubgraph(subgraph)
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            // The stage's frame stamp, not a copy of it: held so that anything reading back from
            // this view can tell how long ago the draw it is waiting for was asked for.
            mFrameStamp = nv->getFrameStamp();

            if (!mPending)
            {
                node->setNodeMask(0);
                return;
            }

            mPending = false;
            mDrawnFrame = nv->getTraversalNumber();

            if (mSubgraph == nullptr)
            {
                traverse(node, nv);
                return;
            }

            // RTTNode does not carry the update traversal into its camera, and a subtree the game
            // built for one picture hangs nowhere else, so its keyframe controllers are updated
            // here or not at all. At simulation time zero, because a doll is a pose and not an
            // animation.
            osg::ref_ptr<osg::FrameStamp> moving = const_cast<osg::FrameStamp*>(nv->getFrameStamp());
            osg::ref_ptr<osg::FrameStamp> still = new osg::FrameStamp(*moving);
            still->setSimulationTime(0.0);

            nv->setFrameStamp(still);
            mSubgraph->accept(*nv);
            traverse(node, nv);
            nv->setFrameStamp(moving);
        }

        void redrawNextFrame() { mPending = true; }

        unsigned int getDrawnFrame() const { return mDrawnFrame; }

        /// Whether the last redraw has been drawn *and* the thread that drew it has moved on, which
        /// is when anything the draw wrote back into main memory can be read.
        bool isDrawDone() const
        {
            if (mPending || mDrawnFrame == 0 || mFrameStamp == nullptr)
                return false;

            // One frame for the draw the cull queued, and one more because the draw thread runs a
            // frame behind the traversal that queued it.
            return mFrameStamp->getFrameNumber() > mDrawnFrame + 1;
        }

    private:
        osg::ref_ptr<osg::Node> mSubgraph;
        osg::ref_ptr<const osg::FrameStamp> mFrameStamp;
        bool mPending = true;
        unsigned int mDrawnFrame = 0;
    };

    /// Rewrites the subtree's blend functions so that what lands in the picture is premultiplied.
    ///
    /// Transparent geometry otherwise writes its own alpha over the alpha already in the target, and
    /// the target is a picture the GUI composites: a half-transparent pauldron punches a hole
    /// through the doll behind it.
    class SetUpBlendVisitor : public osg::NodeVisitor
    {
    public:
        SetUpBlendVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Node& node) override
        {
            if (osg::ref_ptr<osg::StateSet> stateset = node.getStateSet())
            {
                osg::ref_ptr<osg::StateSet> newStateSet;
                if (stateset->getAttribute(osg::StateAttribute::BLENDFUNC)
                    || stateset->getBinNumber() == osg::StateSet::TRANSPARENT_BIN)
                {
                    osg::BlendFunc* blendFunc
                        = static_cast<osg::BlendFunc*>(stateset->getAttribute(osg::StateAttribute::BLENDFUNC));

                    if (blendFunc)
                    {
                        newStateSet = new osg::StateSet(*stateset, osg::CopyOp::SHALLOW_COPY);
                        node.setStateSet(newStateSet);
                        osg::ref_ptr<osg::BlendFunc> newBlendFunc = new osg::BlendFunc(*blendFunc);
                        newStateSet->setAttribute(newBlendFunc, osg::StateAttribute::ON);
                        // I *think* (based on some by-hand maths) that the RGB and dest alpha factors are unchanged,
                        // and only dest determines source alpha factor This has the benefit of being idempotent if we
                        // assume nothing used glBlendFuncSeparate before we touched it
                        if (blendFunc->getDestination() == osg::BlendFunc::ONE_MINUS_SRC_ALPHA)
                            newBlendFunc->setSourceAlpha(osg::BlendFunc::ONE);
                        else if (blendFunc->getDestination() == osg::BlendFunc::ONE)
                            newBlendFunc->setSourceAlpha(osg::BlendFunc::ZERO);
                        // Other setups barely exist in the wild and aren't worth supporting as they're not equippable
                        // gear
                        else
                            Log(Debug::Info) << "Unable to adjust blend mode for character preview. Source factor 0x"
                                             << std::hex << blendFunc->getSource() << ", destination factor 0x"
                                             << blendFunc->getDestination() << std::dec;
                    }
                }
                if (stateset->getMode(GL_BLEND) & osg::StateAttribute::ON)
                {
                    if (!newStateSet)
                    {
                        newStateSet = new osg::StateSet(*stateset, osg::CopyOp::SHALLOW_COPY);
                        node.setStateSet(newStateSet);
                    }
                    newStateSet->setDefine("FORCE_OPAQUE", "0", osg::StateAttribute::ON);
                }
            }
            traverse(node);
        }
    };

    /// The camera, and everything the rasterizer needs above the subtree for it to come out right.
    ///
    /// **`mFromWorld` decides most of what differs between two of these.** A piece of the world
    /// arrives already lit, already placed relative to the eye and already brought up to date by the
    /// frame's own update traversal; a group the game assembled for one picture arrives with none of
    /// that, and is given a light rig, an absolute frame and an update of its own.
    class OffscreenRTTNode : public SceneUtil::RTTNode
    {
    public:
        OffscreenRTTNode(const OffscreenViewSpec& spec, Resource::ResourceSystem& resources)
            : RTTNode(spec.mWidth, spec.mHeight, spec.mFromWorld ? 0 : Settings::video().mAntialiasing, false, 0,
                StereoAwareness::Unaware_MultiViewShaders, shouldAddMSAAIntermediateTarget())
            , mMask(spec.mMask)
            , mClearColour(spec.mClearColour)
            , mFromWorld(spec.mFromWorld)
        {
            setNodeMask(Mask_RenderToTexture);
            setColorBufferInternalFormat(spec.mClearColour.a() < 1.f ? GL_RGBA : GL_RGB);
            setDepthBufferInternalFormat(GL_DEPTH24_STENCIL8);

            buildProjection(spec);
            buildState(spec, resources);
        }

        void setDefaults(osg::Camera* camera) override
        {
            camera->setName("OffscreenView");
            camera->setReferenceFrame(
                mFromWorld ? osg::Camera::ABSOLUTE_RF_INHERIT_VIEWPOINT : osg::Camera::ABSOLUTE_RF);
            camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT, osg::Camera::PIXEL_BUFFER_RTT);
            camera->setClearColor(mClearColour);
            camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            camera->setProjectionMatrix(mCameraProjection);
            camera->setViewport(0, 0, width(), height());
            camera->setRenderOrder(osg::Camera::PRE_RENDER);
            camera->setCullMask(mMask);
            camera->setCullMaskLeft(mMask);
            camera->setCullMaskRight(mMask);
            camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);

            if (mFromWorld)
                // A chart of a whole cell is mostly small features, and the heuristic that drops
                // them was tuned for an eye that moves through the world rather than hangs over it.
                camera->setCullingMode((osg::Camera::DEFAULT_CULLING | osg::Camera::FAR_PLANE_CULLING)
                    & ~osg::Camera::SMALL_FEATURE_CULLING);

            SceneUtil::setCameraClearDepth(camera);

            camera->setNodeMask(Mask_RenderToTexture);
            camera->addChild(mGroup);

            if (mCopy)
                camera->attach(osg::Camera::COLOR_BUFFER, mCopy);
        }

        void apply(osg::Camera* camera) override
        {
            if (mExtentStateSet)
                camera->setStateSet(mExtentStateSet);
            camera->setViewMatrix(mViewMatrix);

            if (shouldDoTextureArray())
                Stereo::setMultiviewMatrices(mGroup->getOrCreateStateSet(), { mShaderProjection, mShaderProjection });
        }

        osg::Group* getContents() { return mGroup; }

        void setViewMatrix(const osg::Matrixf& view) { mViewMatrix = view; }

        void setExtentStateSet(osg::StateSet* stateset) { mExtentStateSet = stateset; }

        /// OpenSceneGraph reads the colour buffer into an attached image after rendering it, which
        /// is the whole of what keeping a copy costs here.
        void copyInto(osg::Image& image)
        {
            mCopy = &image;
            if (osg::Camera* camera = getCamera(nullptr))
                camera->attach(osg::Camera::COLOR_BUFFER, mCopy);
        }

    private:
        /// **The camera and the shaders are given different matrices on purpose.** `AutoDepth` wants
        /// a reversed-Z projection and the shaders read it out of the `projectionMatrix` uniform,
        /// which is the only one that reaches a fragment's depth. The camera keeps the plain form,
        /// because it is what `pick` intersects against and a reversed one would make its
        /// nearest-hit query find the farthest. The two describe the same frustum, so culling does
        /// not care which it gets.
        void buildProjection(const OffscreenViewSpec& spec)
        {
            const bool reversed = SceneUtil::AutoDepth::isReversed();

            if (const auto* perspective = std::get_if<OffscreenViewSpec::Perspective>(&spec.mProjection))
            {
                const double aspect = static_cast<double>(spec.mWidth) / static_cast<double>(spec.mHeight);

                mCameraProjection = osg::Matrixf::perspective(perspective->mFieldOfView, aspect, spec.mNear, spec.mFar);
                mShaderProjection = reversed
                    ? static_cast<osg::Matrixf>(SceneUtil::getReversedZProjectionMatrixAsPerspective(
                        perspective->mFieldOfView, aspect, spec.mNear, spec.mFar))
                    : mCameraProjection;
            }
            else
            {
                const auto& box = std::get<OffscreenViewSpec::Orthographic>(spec.mProjection);
                const double halfWidth = box.mWidth / 2.0;
                const double halfHeight = box.mHeight / 2.0;

                mCameraProjection.makeOrtho(-halfWidth, halfWidth, -halfHeight, halfHeight, spec.mNear, spec.mFar);
                mShaderProjection = reversed ? static_cast<osg::Matrixf>(SceneUtil::getReversedZProjectionMatrixAsOrtho(
                                        -halfWidth, halfWidth, -halfHeight, halfHeight, spec.mNear, spec.mFar))
                                             : mCameraProjection;
            }
        }

        void buildState(const OffscreenViewSpec& spec, Resource::ResourceSystem& resources)
        {
            osg::StateSet* stateset = mGroup->getOrCreateStateSet();
            stateset->addUniform(new osg::Uniform("projectionMatrix", mShaderProjection),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

            // The object shaders fade towards the sky colour past `skyBlendingStart`. There is no
            // sky in this picture, so both distances go past anything that could be in the frame.
            stateset->addUniform(new osg::Uniform("far", 10000000.0f));
            stateset->addUniform(new osg::Uniform("skyBlendingStart", 8000000.0f));
            stateset->addUniform(new osg::Uniform(
                "screenRes", osg::Vec2f{ static_cast<float>(spec.mWidth), static_cast<float>(spec.mHeight) }));

            SceneUtil::disableFog(*stateset, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*stateset);

            osg::ref_ptr<SceneUtil::Light> sun = new SceneUtil::Light;
            sun->setPosition(osg::Vec4f(spec.mSunDirection, 0.f));
            sun->setDiffuse(spec.mSunDiffuse);
            sun->setAmbient(spec.mSunAmbient);
            sun->setSpecular(osg::Vec4f(0.f, 0.f, 0.f, 0.f));
            sun->setConstantAttenuation(1.f);
            sun->setLinearAttenuation(0.f);
            sun->setQuadraticAttenuation(0.f);

            if (mFromWorld)
            {
                // Wireframe is a debug view of the world, not of a chart of it.
                stateset->setAttribute(new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::FILL),
                    osg::StateAttribute::OVERRIDE);

                // The world brought its own light manager; this replaces the sun in it rather than
                // adding a second rig underneath.
                SceneUtil::configureStateSetSunOverride(sun, stateset);

                mGroup->addChild(&spec.mScene);
                return;
            }

            osg::ref_ptr<SceneUtil::LightManager> lights = new SceneUtil::LightManager(
                SceneUtil::LightSettings{
                    .mClusteredLighting = Settings::shaders().mClusteredLighting,
                    .mMaxLights = Settings::shaders().mMaxLights,
                    .mMaximumLightDistance = Settings::shaders().mMaximumLightDistance,
                    .mLightFadeStart = Settings::shaders().mLightFadeStart,
                    .mLightRadiusMultiplier = Settings::shaders().mLightRadiusMultiplier,
                    .mClusteredGridSize = { 1, 1, 1 },
                    .mClusteredWorkGroupSize = 1,
                },
                &resources);
            lights->setSunlight(sun);

            osg::StateSet* lit = lights->getOrCreateStateSet();
            lit->setDefine("FORCE_OPAQUE", "1", osg::StateAttribute::ON);
            lit->setMode(GL_NORMALIZE, osg::StateAttribute::ON);
            lit->setMode(GL_CULL_FACE, osg::StateAttribute::ON);

            osg::ref_ptr<osg::Material> material = new osg::Material;
            material->setColorMode(osg::Material::OFF);
            material->setAmbient(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
            material->setDiffuse(osg::Material::FRONT_AND_BACK, osg::Vec4f(1, 1, 1, 1));
            material->setSpecular(osg::Material::FRONT_AND_BACK, osg::Vec4f(0.f, 0.f, 0.f, 0.f));
            lit->setAttribute(material);

            lit->addUniform(new osg::Uniform("near", spec.mNear));
            lit->addUniform(new osg::Uniform("emissiveMult", 1.f));

            // The shaders sample a shadow map at unit 7 whether or not one was rendered, and an
            // unbound sampler on a bound unit is undefined rather than empty. This one always
            // passes, which is the answer a view with no shadow map wants.
            osg::ref_ptr<osg::Texture2D> alwaysLit = new osg::Texture2D;
            alwaysLit->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            alwaysLit->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            alwaysLit->setInternalFormat(GL_DEPTH_COMPONENT);
            alwaysLit->setTextureSize(1, 1);
            alwaysLit->setShadowComparison(true);
            alwaysLit->setShadowCompareFunc(osg::Texture::ShadowCompareFunc::ALWAYS);
            lit->setTextureAttribute(7, alwaysLit, osg::StateAttribute::ON);

            lights->addChild(&spec.mScene);
            mGroup->addChild(lights);
        }

        osg::ref_ptr<osg::Group> mGroup = new osg::Group;
        osg::ref_ptr<osg::StateSet> mExtentStateSet;
        osg::ref_ptr<osg::Image> mCopy;
        osg::Matrixf mCameraProjection;
        osg::Matrixf mShaderProjection;
        osg::Matrixf mViewMatrix = osg::Matrixf::identity();
        unsigned int mMask;
        osg::Vec4f mClearColour;
        bool mFromWorld;
    };

    GlOffscreenView::GlOffscreenView(
        const OffscreenViewSpec& spec, osg::Group& parent, Resource::ResourceSystem& resources)
        : mParent(&parent)
        , mScene(&spec.mScene)
        , mTransparent(spec.mClearColour.a() < 1.f)
    {
        mNode = new OffscreenRTTNode(spec, resources);
        mDrawOnce = new OffscreenDrawOnceCallback(spec.mFromWorld ? nullptr : mNode->getContents());
        mNode->addUpdateCallback(mDrawOnce);
        mParent->addChild(mNode);

        osg::ref_ptr<osg::StateSet> premultiplied;
        if (mTransparent)
        {
            // The picture is premultiplied — see SetUpBlendVisitor — so the source factor is one
            // rather than the widget's usual source alpha.
            premultiplied = new osg::StateSet;
            premultiplied->setAttribute(new osg::BlendFunc(osg::BlendFunc::ONE, osg::BlendFunc::ONE_MINUS_SRC_ALPHA));
        }

        mTexture = std::make_unique<MyGUIPlatform::OSGTexture>(
            static_cast<osg::Texture2D*>(mNode->getColorTexture(nullptr)), premultiplied);
    }

    GlOffscreenView::~GlOffscreenView()
    {
        mParent->removeChild(mNode);
    }

    MyGUI::ITexture& GlOffscreenView::getTexture() const
    {
        return *mTexture;
    }

    void GlOffscreenView::setView(const osg::Matrixf& view)
    {
        mNode->setViewMatrix(view);
    }

    void GlOffscreenView::setExtent(int width, int height)
    {
        const int filledX = std::clamp(width, 0, static_cast<int>(mNode->width()));
        const int filledY = std::clamp(height, 0, static_cast<int>(mNode->height()));

        // NB Camera::setViewport has threading issues
        osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;
        // This expects Y-down convention; historically the origin was (0, mSizeY - sizeY)
        stateset->setAttributeAndModes(new osg::Viewport(0, 0, filledX, filledY));
        mNode->setExtentStateSet(stateset);
    }

    void GlOffscreenView::sceneChanged()
    {
        if (!mTransparent)
            return;

        SetUpBlendVisitor visitor;
        mScene->accept(visitor);
    }

    void GlOffscreenView::redraw()
    {
        mNode->setNodeMask(Mask_RenderToTexture);
        mDrawOnce->redrawNextFrame();
    }

    void GlOffscreenView::keepCopy()
    {
        if (mCopy)
            return;

        mCopy = new osg::Image;
        mCopy->setPixelFormat(GL_RGBA);
        mCopy->setDataType(GL_UNSIGNED_BYTE);
        mNode->copyInto(*mCopy);
    }

    const osg::Image* GlOffscreenView::getCopy() const
    {
        if (!mCopy || !mCopy->valid() || !mDrawOnce->isDrawDone())
            return nullptr;

        return mCopy;
    }

    bool GlOffscreenView::pick(float x, float y, osg::NodePath& hit) const
    {
        // With Intersector::WINDOW, the intersection ratios are slightly inaccurate. Seems to be a
        // precision issue - compiling with OSG_USE_FLOAT_MATRIX=0, Intersector::WINDOW works ok.
        // Using Intersector::PROJECTION results in better precision because the start/end points and the model matrices
        // don't go through as many transformations.
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector
            = new osgUtil::LineSegmentIntersector(osgUtil::Intersector::PROJECTION, x, y);
        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);

        osgUtil::IntersectionVisitor visitor(intersector);
        visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);
        // Set the traversal number from the last draw, so that the frame switch used for RigGeometry double buffering
        // works correctly
        visitor.setTraversalNumber(mDrawOnce->getDrawnFrame());

        osg::Camera* camera = mNode->getCamera(nullptr);
        const osg::Node::NodeMask mask = camera->getNodeMask();
        camera->setNodeMask(~0u);
        camera->accept(visitor);
        camera->setNodeMask(mask);

        if (!intersector->containsIntersections())
            return false;

        hit = intersector->getFirstIntersection().nodePath;
        return true;
    }

}

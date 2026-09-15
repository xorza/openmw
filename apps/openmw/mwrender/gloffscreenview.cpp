#include "gloffscreenview.hpp"

#include <algorithm>
#include <cassert>
#include <variant>

#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Group>
#include <osg/Image>
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
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/rtt.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/values.hpp>
#include <components/stereo/multiview.hpp>

#include "util.hpp"
#include "vismask.hpp"

namespace MWRender
{
    // Upstream's, from characterpreview.cpp. `isPending` is the one addition.
    class DrawOnceCallback : public SceneUtil::NodeCallback<DrawOnceCallback>
    {
    public:
        DrawOnceCallback(osg::Node* subgraph)
            : mRendered(false)
            , mLastRenderedFrame(0)
            , mSubgraph(subgraph)
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            if (!mRendered)
            {
                mRendered = true;

                mLastRenderedFrame = nv->getTraversalNumber();

                osg::ref_ptr<osg::FrameStamp> previousFramestamp = const_cast<osg::FrameStamp*>(nv->getFrameStamp());
                osg::FrameStamp* fs = new osg::FrameStamp(*previousFramestamp);
                fs->setSimulationTime(0.0);

                nv->setFrameStamp(fs);

                // Update keyframe controllers in the scene graph first...
                // RTTNode does not continue update traversal, so manually continue the update traversal since we need
                // it.
                mSubgraph->accept(*nv);
                traverse(node, nv);

                nv->setFrameStamp(previousFramestamp);
            }
            else
            {
                node->setNodeMask(0);
            }
        }

        void redrawNextFrame() { mRendered = false; }

        unsigned int getLastRenderedFrame() const { return mLastRenderedFrame; }

        bool isPending() const { return !mRendered; }

    private:
        bool mRendered;
        unsigned int mLastRenderedFrame;
        osg::ref_ptr<osg::Node> mSubgraph;
    };

    // Upstream's, from characterpreview.cpp.
    // Set up alpha blending mode to avoid issues caused by transparent objects writing onto the alpha value of the FBO
    // This makes the RTT have premultiplied alpha, though, so the source blend factor must be GL_ONE when it's applied
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

    // Upstream's, from characterpreview.cpp. The size, the field of view, the clip planes, the cull
    // mask and the clear colour come out of the spec where upstream passed or hard-coded them; every
    // other line is as it was.
    class CharacterPreviewRTTNode : public SceneUtil::RTTNode
    {
    public:
        CharacterPreviewRTTNode(const OffscreenViewSpec& spec)
            : RTTNode(spec.mWidth, spec.mHeight, Settings::video().mAntialiasing, false, 0,
                StereoAwareness::Unaware_MultiViewShaders, shouldAddMSAAIntermediateTarget())
            , mAspectRatio(static_cast<float>(spec.mWidth) / static_cast<float>(spec.mHeight))
            , fovYDegrees(std::get<SceneUtil::Perspective>(spec.mFraming.mProjection).mFieldOfView)
            , znear(spec.mFraming.mNear)
            , zfar(spec.mFraming.mFar)
            , mMask(spec.mMask)
            , mClearColour(spec.mClearColour)
        {
            if (SceneUtil::AutoDepth::isReversed())
                mPerspectiveMatrix = static_cast<osg::Matrixf>(
                    SceneUtil::getReversedZProjectionMatrixAsPerspective(fovYDegrees, mAspectRatio, znear, zfar));
            else
                mPerspectiveMatrix = osg::Matrixf::perspective(fovYDegrees, mAspectRatio, znear, zfar);
            mGroup->getOrCreateStateSet()->addUniform(new osg::Uniform("projectionMatrix", mPerspectiveMatrix));
            mViewMatrix = osg::Matrixf::identity();
            setColorBufferInternalFormat(GL_RGBA);
            setDepthBufferInternalFormat(GL_DEPTH24_STENCIL8);
        }

        void setDefaults(osg::Camera* camera) override
        {
            camera->setName("CharacterPreview");
            camera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
            camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT, osg::Camera::PIXEL_BUFFER_RTT);
            camera->setClearColor(mClearColour);
            camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            camera->setProjectionMatrixAsPerspective(fovYDegrees, mAspectRatio, znear, zfar);
            camera->setViewport(0, 0, width(), height());
            camera->setRenderOrder(osg::Camera::PRE_RENDER);
            camera->setCullMask(mMask);
            camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
            SceneUtil::setCameraClearDepth(camera);

            camera->setNodeMask(Mask_RenderToTexture);
            camera->addChild(mGroup);
        }

        void apply(osg::Camera* camera) override
        {
            if (mCameraStateset)
                camera->setStateSet(mCameraStateset);
            camera->setViewMatrix(mViewMatrix);

            if (shouldDoTextureArray())
                Stereo::setMultiviewMatrices(mGroup->getOrCreateStateSet(), { mPerspectiveMatrix, mPerspectiveMatrix });
        }

        void addChild(osg::Node* node) { mGroup->addChild(node); }

        void setCameraStateset(osg::StateSet* stateset) { mCameraStateset = stateset; }

        void setViewMatrix(const osg::Matrixf& viewMatrix) { mViewMatrix = viewMatrix; }

        osg::ref_ptr<osg::Group> mGroup = new osg::Group;
        osg::Matrixf mPerspectiveMatrix;
        osg::Matrixf mViewMatrix;
        osg::ref_ptr<osg::StateSet> mCameraStateset;
        float mAspectRatio;

        const float fovYDegrees;
        const float znear;
        const float zfar;
        const unsigned int mMask;
        const osg::Vec4f mClearColour;
    };

    // Upstream's, from localmap.cpp. The size, the box, the clip planes, the cull mask, the clear
    // colour and the sun come out of the spec where upstream passed or hard-coded them; the view
    // matrix arrives through `setView` rather than the constructor, since the spec does not carry
    // one; `redraw` and the drawn frame are what the view over this needs back.
    class LocalMapRenderToTexture : public SceneUtil::RTTNode
    {
    public:
        LocalMapRenderToTexture(const OffscreenViewSpec& spec);

        void setDefaults(osg::Camera* camera) override;
        void apply(osg::Camera* camera) override;

        void redraw();

        osg::Node* mSceneRoot;
        osg::Matrix mProjectionMatrix;
        osg::Matrix mViewMatrix;
        bool mActive;
        unsigned int mLastRenderedFrame = 0;

        const unsigned int mMask;
        const osg::Vec4f mClearColour;
        const SceneUtil::FlatLight mSun;
    };

    class CameraLocalUpdateCallback
        : public SceneUtil::NodeCallback<CameraLocalUpdateCallback, LocalMapRenderToTexture*>
    {
    public:
        void operator()(LocalMapRenderToTexture* node, osg::NodeVisitor* nv);
    };

    namespace
    {
        // Upstream's, from CharacterPreview's constructor: the rig a subtree the game assembled is
        // lit by, with the sun's colour and direction out of the spec where upstream read fallbacks.
        osg::ref_ptr<SceneUtil::LightManager> buildDollLighting(
            const OffscreenViewSpec& spec, Resource::ResourceSystem& resources)
        {
            osg::ref_ptr<SceneUtil::LightManager> lightManager = new SceneUtil::LightManager(
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
            osg::ref_ptr<osg::StateSet> stateset = lightManager->getOrCreateStateSet();
            stateset->setDefine("FORCE_OPAQUE", "1", osg::StateAttribute::ON);
            stateset->setMode(GL_NORMALIZE, osg::StateAttribute::ON);
            stateset->setMode(GL_CULL_FACE, osg::StateAttribute::ON);
            osg::ref_ptr<SceneUtil::Material> defaultMat(new SceneUtil::Material);
            defaultMat->updateStateSet(stateset);
            stateset->setAttribute(defaultMat);

            SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*stateset);

            SceneUtil::disableFog(*stateset, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

            // TODO: Clean up this mess of loose uniforms that shaders depend on.
            // turn off sky blending
            stateset->addUniform(new osg::Uniform("far", 10000000.0f));
            stateset->addUniform(new osg::Uniform("near", spec.mFraming.mNear));
            stateset->addUniform(new osg::Uniform("skyBlendingStart", 8000000.0f));
            stateset->addUniform(new osg::Uniform(
                "screenRes", osg::Vec2f{ static_cast<float>(spec.mWidth), static_cast<float>(spec.mHeight) }));
            stateset->addUniform(new osg::Uniform("alpha", 1.f));
            stateset->addUniform(new osg::Uniform("actorFade", 1.f));

            osg::ref_ptr<osg::Texture2D> dummyTexture = new osg::Texture2D();
            dummyTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            dummyTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            dummyTexture->setInternalFormat(GL_DEPTH_COMPONENT);
            dummyTexture->setTextureSize(1, 1);
            // This might clash with a shadow map, so make sure it doesn't cast shadows
            dummyTexture->setShadowComparison(true);
            dummyTexture->setShadowCompareFunc(osg::Texture::ShadowCompareFunc::ALWAYS);
            stateset->setTextureAttribute(7, dummyTexture, osg::StateAttribute::ON);

            osg::ref_ptr<SceneUtil::Light> light = new SceneUtil::Light;
            light->setPosition(osg::Vec4(spec.mSun.mDirection, 0.0));
            light->setDiffuse(spec.mSun.mDiffuse);
            light->setAmbient(spec.mSun.mAmbient);
            light->setSpecular(osg::Vec4(0, 0, 0, 0));
            light->setConstantAttenuation(1.f);
            light->setLinearAttenuation(0.f);
            light->setQuadraticAttenuation(0.f);
            lightManager->setSunlight(light);

            return lightManager;
        }

        // Upstream's `CharacterPreview::mTextureStateSet`: the picture is premultiplied — see
        // SetUpBlendVisitor — so the source factor is one rather than the widget's usual source alpha.
        osg::ref_ptr<osg::StateSet> premultipliedBlend()
        {
            osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;
            stateset->setAttribute(new osg::BlendFunc(osg::BlendFunc::ONE, osg::BlendFunc::ONE_MINUS_SRC_ALPHA));
            return stateset;
        }
    }

    GlOffscreenView::GlOffscreenView(
        SceneUtil::RTTNode& node, osg::Group& parent, const osg::FrameStamp& frameStamp, osg::StateSet* blend)
        : mParent(&parent)
        , mNode(&node)
        , mFrameStamp(frameStamp)
    {
        mParent->addChild(mNode);

        // Asking for the texture is what makes the camera, so from here on there is one to attach
        // a copy to and to pick against.
        mTexture = std::make_unique<MyGUIPlatform::OSGTexture>(
            static_cast<osg::Texture2D*>(mNode->getColorTexture(nullptr)), blend);
    }

    GlOffscreenView::~GlOffscreenView()
    {
        mParent->removeChild(mNode);
    }

    MyGUI::ITexture& GlOffscreenView::getTexture() const
    {
        return *mTexture;
    }

    void GlOffscreenView::keepCopy()
    {
        if (mCopy)
            return;

        // OpenSceneGraph reads the colour buffer into an attached image after rendering it, which
        // is the whole of what keeping a copy costs.
        mCopy = new osg::Image;
        mCopy->setPixelFormat(GL_RGBA);
        mCopy->setDataType(GL_UNSIGNED_BYTE);
        mNode->getCamera(nullptr)->attach(osg::Camera::COLOR_BUFFER, mCopy);

        // A draw already made carried no copy; one still pending will carry this one
        if (!isPending())
            redraw();
    }

    const osg::Image* GlOffscreenView::getCopy()
    {
        if (!mCopy || !mCopy->valid() || isPending() || getDrawnFrame() == 0)
            return nullptr;

        // One frame for the draw the cull queued, and one more because the draw thread runs a
        // frame behind the traversal that queued it.
        if (mFrameStamp.getFrameNumber() <= getDrawnFrame() + 1)
            return nullptr;

        return mCopy;
    }

    // Upstream's, from InventoryPreview::getSlotSelected.
    bool GlOffscreenView::pick(float x, float y, osg::NodePath& hit) const
    {
        // With Intersector::WINDOW, the intersection ratios are slightly inaccurate. Seems to be a
        // precision issue - compiling with OSG_USE_FLOAT_MATRIX=0, Intersector::WINDOW works ok.
        // Using Intersector::PROJECTION results in better precision because the start/end points and the model matrices
        // don't go through as many transformations.
        osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector(
            new osgUtil::LineSegmentIntersector(osgUtil::Intersector::PROJECTION, x, y));

        intersector->setIntersectionLimit(osgUtil::LineSegmentIntersector::LIMIT_NEAREST);
        osgUtil::IntersectionVisitor visitor(intersector);
        visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);
        // Set the traversal number from the last draw, so that the frame switch used for RigGeometry double buffering
        // works correctly
        visitor.setTraversalNumber(getDrawnFrame());

        auto* camera = mNode->getCamera(nullptr);
        osg::Node::NodeMask nodeMask = camera->getNodeMask();
        camera->setNodeMask(~0u);
        camera->accept(visitor);
        camera->setNodeMask(nodeMask);

        if (!intersector->containsIntersections())
            return false;

        hit = intersector->getFirstIntersection().nodePath;
        return true;
    }

    GlDollView::GlDollView(const OffscreenViewSpec& spec, osg::Group& parent, const osg::FrameStamp& frameStamp,
        Resource::ResourceSystem& resources)
        : GlDollView(spec, *new CharacterPreviewRTTNode(spec), parent, frameStamp, resources)
    {
    }

    // Upstream's, from CharacterPreview's constructor, with the spec's subtree where it made its own.
    GlDollView::GlDollView(const OffscreenViewSpec& spec, CharacterPreviewRTTNode& node, osg::Group& parent,
        const osg::FrameStamp& frameStamp, Resource::ResourceSystem& resources)
        : GlOffscreenView(node, parent, frameStamp, premultipliedBlend())
        , mDoll(node)
        , mScene(&spec.mScene)
    {
        mDoll.setNodeMask(Mask_RenderToTexture);

        osg::ref_ptr<SceneUtil::LightManager> lightManager = buildDollLighting(spec, resources);

        mDoll.addChild(lightManager);

        lightManager->addChild(mScene);

        mDrawOnce = new DrawOnceCallback(mDoll.mGroup);
        mDoll.addUpdateCallback(mDrawOnce);
    }

    GlDollView::~GlDollView() = default;

    void GlDollView::setView(const osg::Matrixf& view)
    {
        mDoll.setViewMatrix(view);
    }

    // Upstream's, from InventoryPreview::setViewport; the redraw is the caller's.
    void GlDollView::setExtent(int width, int height)
    {
        const int sizeX = static_cast<int>(mDoll.width());
        const int sizeY = static_cast<int>(mDoll.height());

        // NB Camera::setViewport has threading issues
        osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;
        // This expects Y-down convention; historically the origin was (0, mSizeY - sizeY)
        stateset->setAttributeAndModes(
            new osg::Viewport(0, 0, std::min(sizeX, std::max(width, 0)), std::min(sizeY, std::max(height, 0))));
        mDoll.setCameraStateset(stateset);
    }

    void GlDollView::sceneChanged()
    {
        SetUpBlendVisitor visitor;
        mScene->accept(visitor);
    }

    void GlDollView::redraw()
    {
        mDoll.setNodeMask(Mask_RenderToTexture);
        mDrawOnce->redrawNextFrame();
    }

    unsigned int GlDollView::getDrawnFrame() const
    {
        return mDrawOnce->getLastRenderedFrame();
    }

    bool GlDollView::isPending() const
    {
        return mDrawOnce->isPending();
    }

    GlTileView::GlTileView(const OffscreenViewSpec& spec, osg::Group& parent, const osg::FrameStamp& frameStamp)
        : GlTileView(*new LocalMapRenderToTexture(spec), parent, frameStamp)
    {
    }

    GlTileView::GlTileView(LocalMapRenderToTexture& node, osg::Group& parent, const osg::FrameStamp& frameStamp)
        : GlOffscreenView(node, parent, frameStamp, nullptr)
        , mTile(node)
    {
    }

    void GlTileView::setView(const osg::Matrixf& view)
    {
        mTile.mViewMatrix = view;
    }

    void GlTileView::setExtent(int width, int height)
    {
        assert(width == static_cast<int>(mTile.width()) && height == static_cast<int>(mTile.height())
            && "a map tile is drawn whole");
    }

    void GlTileView::redraw()
    {
        mTile.redraw();
    }

    unsigned int GlTileView::getDrawnFrame() const
    {
        return mTile.mLastRenderedFrame;
    }

    bool GlTileView::isPending() const
    {
        return mTile.mActive;
    }

    LocalMapRenderToTexture::LocalMapRenderToTexture(const OffscreenViewSpec& spec)
        : RTTNode(spec.mWidth, spec.mHeight, 0, false, 0, StereoAwareness::Unaware_MultiViewShaders,
            shouldAddMSAAIntermediateTarget())
        , mSceneRoot(&spec.mScene)
        , mActive(true)
        , mMask(spec.mMask)
        , mClearColour(spec.mClearColour)
        , mSun(spec.mSun)
    {
        setNodeMask(Mask_RenderToTexture);

        const SceneUtil::Orthographic& box = std::get<SceneUtil::Orthographic>(spec.mFraming.mProjection);
        const float near = spec.mFraming.mNear;
        const float far = spec.mFraming.mFar;

        if (SceneUtil::AutoDepth::isReversed())
            mProjectionMatrix = SceneUtil::getReversedZProjectionMatrixAsOrtho(
                -box.mWidth / 2, box.mWidth / 2, -box.mHeight / 2, box.mHeight / 2, near, far);
        else
            mProjectionMatrix.makeOrtho(-box.mWidth / 2, box.mWidth / 2, -box.mHeight / 2, box.mHeight / 2, near, far);

        setUpdateCallback(new CameraLocalUpdateCallback);
        setDepthBufferInternalFormat(GL_DEPTH24_STENCIL8);
    }

    void LocalMapRenderToTexture::setDefaults(osg::Camera* camera)
    {
        // Disable small feature culling, it's not going to be reliable for this camera
        osg::Camera::CullingMode cullingMode
            = (osg::Camera::DEFAULT_CULLING | osg::Camera::FAR_PLANE_CULLING) & ~(osg::Camera::SMALL_FEATURE_CULLING);
        camera->setCullingMode(cullingMode);

        SceneUtil::setCameraClearDepth(camera);
        camera->setComputeNearFarMode(osg::Camera::DO_NOT_COMPUTE_NEAR_FAR);
        camera->setReferenceFrame(osg::Camera::ABSOLUTE_RF_INHERIT_VIEWPOINT);
        camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT, osg::Camera::PIXEL_BUFFER_RTT);
        camera->setClearColor(mClearColour);
        camera->setClearMask(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        camera->setRenderOrder(osg::Camera::PRE_RENDER);

        camera->setCullMask(mMask);
        camera->setCullMaskLeft(mMask);
        camera->setCullMaskRight(mMask);
        camera->setNodeMask(Mask_RenderToTexture);
        camera->setProjectionMatrix(mProjectionMatrix);
        camera->setViewMatrix(mViewMatrix);

        auto* stateset = camera->getOrCreateStateSet();

        stateset->setAttribute(new osg::PolygonMode(osg::PolygonMode::FRONT_AND_BACK, osg::PolygonMode::FILL),
            osg::StateAttribute::OVERRIDE);
        stateset->addUniform(new osg::Uniform("projectionMatrix", static_cast<osg::Matrixf>(mProjectionMatrix)),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

        if (Stereo::getMultiview())
            Stereo::setMultiviewMatrices(stateset, { mProjectionMatrix, mProjectionMatrix });

        SceneUtil::disableFog(*stateset, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);

        // turn of sky blending
        stateset->addUniform(new osg::Uniform("far", 10000000.0f));
        stateset->addUniform(new osg::Uniform("skyBlendingStart", 8000000.0f));
        stateset->addUniform(new osg::Uniform("screenRes", osg::Vec2f{ 1, 1 }));

        osg::ref_ptr<SceneUtil::Light> light = new SceneUtil::Light;
        light->setPosition(osg::Vec4(mSun.mDirection, 0.f));
        light->setDiffuse(mSun.mDiffuse);
        light->setAmbient(mSun.mAmbient);
        light->setSpecular(osg::Vec4(0, 0, 0, 0));
        light->setConstantAttenuation(1.f);
        light->setLinearAttenuation(0.f);
        light->setQuadraticAttenuation(0.f);

        SceneUtil::ShadowManager::instance().disableShadowsForStateSet(*stateset);

        // override sun for local map
        SceneUtil::configureStateSetSunOverride(light, stateset);

        camera->addChild(mSceneRoot);
    }

    // The camera is made before the first `setView`, so the matrix is applied per cull rather than
    // once at creation.
    void LocalMapRenderToTexture::apply(osg::Camera* camera)
    {
        camera->setViewMatrix(mViewMatrix);
    }

    void LocalMapRenderToTexture::redraw()
    {
        mActive = true;
        setNodeMask(Mask_RenderToTexture);
    }

    void CameraLocalUpdateCallback::operator()(LocalMapRenderToTexture* node, osg::NodeVisitor* nv)
    {
        if (!node->mActive)
            node->setNodeMask(0);
        else
            node->mLastRenderedFrame = nv->getTraversalNumber();

        node->mActive = false;

        // Rtt-nodes do not forward update traversal to their cameras so we can traverse safely.
        // Traverse in case there are nested callbacks.
        traverse(node, nv);
    }
}

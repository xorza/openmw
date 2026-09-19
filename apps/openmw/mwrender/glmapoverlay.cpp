#include "glmapoverlay.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>

#include <osg/Camera>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/myguiplatform/myguitexture.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/shader/shadermanager.hpp>

#include "gloffscreenview.hpp"
#include "glrenderer.hpp"
#include "vismask.hpp"

namespace
{

    // Create a screen-aligned quad with given texture coordinates.
    // Assumes a top-left origin of the sampled image.
    osg::ref_ptr<osg::Geometry> createTexturedQuad(
        float leftTexCoord, float topTexCoord, float rightTexCoord, float bottomTexCoord)
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;

        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array;
        verts->push_back(osg::Vec3f(-1, -1, 0));
        verts->push_back(osg::Vec3f(-1, 1, 0));
        verts->push_back(osg::Vec3f(1, 1, 0));
        verts->push_back(osg::Vec3f(1, -1, 0));

        geom->setVertexArray(verts);

        osg::ref_ptr<osg::Vec2Array> texcoords = new osg::Vec2Array;
        texcoords->push_back(osg::Vec2f(leftTexCoord, 1.f - bottomTexCoord));
        texcoords->push_back(osg::Vec2f(leftTexCoord, 1.f - topTexCoord));
        texcoords->push_back(osg::Vec2f(rightTexCoord, 1.f - topTexCoord));
        texcoords->push_back(osg::Vec2f(rightTexCoord, 1.f - bottomTexCoord));

        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        colors->push_back(osg::Vec4(1.f, 1.f, 1.f, 1.f));
        geom->setColorArray(colors, osg::Array::BIND_OVERALL);

        geom->setTexCoordArray(0, texcoords, osg::Array::BIND_PER_VERTEX);

        geom->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, 4));

        return geom;
    }

    class CameraUpdateGlobalCallback : public SceneUtil::NodeCallback<CameraUpdateGlobalCallback, osg::Camera*>
    {
    public:
        CameraUpdateGlobalCallback(MWRender::GlMapOverlay* parent)
            : mRendered(false)
            , mParent(parent)
        {
        }

        void operator()(osg::Camera* node, osg::NodeVisitor* nv)
        {
            if (mRendered)
            {
                if (mParent->copyResult(node, nv->getTraversalNumber()))
                {
                    node->setNodeMask(0);
                    mParent->markForRemoval(node);
                }
                return;
            }

            traverse(node, nv);

            mRendered = true;
        }

    private:
        bool mRendered;
        MWRender::GlMapOverlay* mParent;
    };

    /// A texture over `image`, clamped and filtered as every texture of the map is.
    osg::ref_ptr<osg::Texture2D> textureOver(osg::ref_ptr<osg::Image> image)
    {
        osg::ref_ptr<osg::Texture2D> texture(new osg::Texture2D);
        texture->setImage(std::move(image));
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        texture->setResizeNonPowerOfTwoHint(false);
        return texture;
    }
}

namespace MWRender
{
    GlMapOverlay::GlMapOverlay(
        const MapOverlaySpec& spec, osg::Group& root, Resource::ResourceSystem& resources, GlRenderer& renderer)
        : mRenderer(renderer)
        , mResources(resources)
        , mWidth(spec.mWidth)
        , mHeight(spec.mHeight)
        , mRoot(new osg::Group)
    {
        root.addChild(mRoot);

        // Bind a dummy alpha texture at top of map subgraph
        osg::ref_ptr<osg::Image> fallbackImage = new osg::Image;
        fallbackImage->allocateImage(1, 1, 1, GL_ALPHA, GL_UNSIGNED_BYTE);
        *fallbackImage->data(0, 0) = 0xFF;

        osg::ref_ptr<osg::Texture2D> dummyTex = new osg::Texture2D(fallbackImage);
        dummyTex->setWrap(osg::Texture2D::WRAP_S, osg::Texture2D::REPEAT);
        dummyTex->setWrap(osg::Texture2D::WRAP_T, osg::Texture2D::REPEAT);
        dummyTex->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture2D::NEAREST);
        dummyTex->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture2D::NEAREST);
        dummyTex->setInternalFormat(GL_ALPHA);

        mRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("alphaMap", 1));
        mRoot->getOrCreateStateSet()->setTextureAttribute(1, dummyTex);

        // Upstream's, from CreateMapWorkItem::doWork: the alpha over the land, the overlay's image
        // and the texture the cameras draw into.
        mAlphaTexture = textureOver(spec.mLandAlpha);

        mOverlayImage = new osg::Image;
        mOverlayImage->allocateImage(mWidth, mHeight, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        assert(mOverlayImage->isDataContiguous());

        memset(mOverlayImage->data(), 0, mOverlayImage->getTotalSizeInBytes());

        mOverlayTexture = new osg::Texture2D;
        mOverlayTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mOverlayTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mOverlayTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        mOverlayTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        mOverlayTexture->setResizeNonPowerOfTwoHint(false);
        mOverlayTexture->setInternalFormat(GL_RGBA);
        mOverlayTexture->setTextureSize(mWidth, mHeight);

        // Upstream's, from GlobalMap::ensureLoaded: the texture cleared before anything is painted.
        requestOverlayTextureUpdate(0, 0, mWidth, mHeight, osg::ref_ptr<osg::Texture2D>(), true, false);

        mGuiTexture = std::make_unique<MyGUIPlatform::OSGTexture>(mOverlayTexture.get());

        mRenderer.setMapOverlay(this);
    }

    GlMapOverlay::~GlMapOverlay()
    {
        mRenderer.setMapOverlay(nullptr);

        for (auto& camera : mCamerasPendingRemoval)
            removeCamera(camera);
        for (auto& camera : mActiveCameras)
            removeCamera(camera);

        mRoot->getParent(0)->removeChild(mRoot);
    }

    void GlMapOverlay::requestOverlayTextureUpdate(int x, int y, int width, int height,
        osg::ref_ptr<osg::Texture2D> texture, bool clear, bool cpuCopy, float srcLeft, float srcTop, float srcRight,
        float srcBottom)
    {
        osg::ref_ptr<osg::Camera> camera(new osg::Camera);
        camera->setNodeMask(Mask_RenderToTexture);
        camera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
        camera->setViewMatrix(osg::Matrix::identity());
        camera->setProjectionMatrix(osg::Matrix::identity());
        camera->setProjectionResizePolicy(osg::Camera::FIXED);
        camera->setRenderOrder(osg::Camera::PRE_RENDER, 1); // Make sure the global map is rendered after the local map
        y = mHeight - y - height; // convert top-left origin to bottom-left
        camera->setViewport(x, y, width, height);

        if (clear)
        {
            camera->setClearMask(GL_COLOR_BUFFER_BIT);
            camera->setClearColor(osg::Vec4(0, 0, 0, 0));
        }
        else
            camera->setClearMask(GL_NONE);

        camera->setUpdateCallback(new CameraUpdateGlobalCallback(this));

        camera->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT, osg::Camera::PIXEL_BUFFER_RTT);
        camera->attach(osg::Camera::COLOR_BUFFER, mOverlayTexture);

        // no need for a depth buffer
        camera->setImplicitBufferAttachmentMask(osg::DisplaySettings::IMPLICIT_COLOR_BUFFER_ATTACHMENT);

        if (cpuCopy)
        {
            // Attach an image to copy the render back to the CPU when finished
            osg::ref_ptr<osg::Image> image(new osg::Image);
            image->setPixelFormat(mOverlayImage->getPixelFormat());
            image->setDataType(mOverlayImage->getDataType());
            camera->attach(osg::Camera::COLOR_BUFFER, image);

            ImageDest imageDest;
            imageDest.mImage = image;
            imageDest.mX = x;
            imageDest.mY = y;
            mPendingImageDest[camera] = std::move(imageDest);
        }

        // Create a quad rendering the updated texture
        if (texture)
        {
            osg::ref_ptr<osg::Geometry> geom = createTexturedQuad(srcLeft, srcTop, srcRight, srcBottom);
            osg::ref_ptr<osg::Depth> depth = new SceneUtil::AutoDepth;
            depth->setWriteMask(false);
            osg::StateSet* stateset = geom->getOrCreateStateSet();
            stateset->setAttribute(depth);
            stateset->setTextureAttribute(0, texture, osg::StateAttribute::ON);
            stateset->addUniform(new osg::Uniform("diffuseMap", 0));
            stateset->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);

            if (mAlphaTexture)
            {
                osg::ref_ptr<osg::Vec2Array> texcoords = new osg::Vec2Array;

                float x1 = x / static_cast<float>(mWidth);
                float x2 = (x + width) / static_cast<float>(mWidth);
                float y1 = y / static_cast<float>(mHeight);
                float y2 = (y + height) / static_cast<float>(mHeight);
                texcoords->push_back(osg::Vec2f(x1, y1));
                texcoords->push_back(osg::Vec2f(x1, y2));
                texcoords->push_back(osg::Vec2f(x2, y2));
                texcoords->push_back(osg::Vec2f(x2, y1));
                geom->setTexCoordArray(1, texcoords, osg::Array::BIND_PER_VERTEX);

                stateset->setTextureAttribute(1, mAlphaTexture, osg::StateAttribute::ON);
            }

            auto& shaderManager = mResources.getSceneManager()->getShaderManager();

            geom->getOrCreateStateSet()->setAttributeAndModes(shaderManager.getProgram("globalmap"));

            camera->addChild(geom);
        }

        mRoot->addChild(camera);

        mActiveCameras.push_back(camera);
    }

    void GlMapOverlay::paintTile(const SceneUtil::ImageRegion& destination, std::shared_ptr<OffscreenView> tile)
    {
        // The seam counts rows from the bottom and the request from the top.
        const int top = mHeight - destination.mY - destination.mHeight;

        // Upstream's GlobalMap::exploreCell, handed the local map's texture: a tile this renderer
        // made is a GlTileView, whose texture is the one upstream's `getMapTexture` answered with.
        osg::ref_ptr<osg::Texture2D> texture = &static_cast<GlTileView&>(*tile).getColorTexture();
        requestOverlayTextureUpdate(
            destination.mX, top, destination.mWidth, destination.mHeight, std::move(texture), false, true);
    }

    void GlMapOverlay::paintImage(
        const SceneUtil::ImageRegion& destination, osg::ref_ptr<osg::Image> image, const SceneUtil::ImageRegion& source)
    {
        // Upstream's GlobalMap::read, where the save's size or region differs: a camera draws the
        // image onto the overlay on the next frame, filtered, and the result is copied back.
        const float imageWidth = static_cast<float>(image->s());
        const float imageHeight = static_cast<float>(image->t());
        const int top = mHeight - destination.mY - destination.mHeight;
        const float srcTop = (imageHeight - static_cast<float>(source.mY + source.mHeight)) / imageHeight;
        const float srcBottom = (imageHeight - static_cast<float>(source.mY)) / imageHeight;

        requestOverlayTextureUpdate(destination.mX, top, destination.mWidth, destination.mHeight,
            textureOver(std::move(image)), true, true, static_cast<float>(source.mX) / imageWidth, srcTop,
            static_cast<float>(source.mX + source.mWidth) / imageWidth, srcBottom);
    }

    void GlMapOverlay::replace(osg::ref_ptr<osg::Image> image)
    {
        // Upstream's GlobalMap::read, where the save's size matches.
        mOverlayImage = image;

        requestOverlayTextureUpdate(0, 0, mWidth, mHeight, textureOver(std::move(image)), true, false);
    }

    void GlMapOverlay::clear()
    {
        memset(mOverlayImage->data(), 0, mOverlayImage->getTotalSizeInBytes());

        mPendingImageDest.clear();

        // just push a Camera to clear the FBO, instead of setImage()/dirty()
        // easier, since we don't need to worry about synchronizing access :)
        requestOverlayTextureUpdate(0, 0, mWidth, mHeight, osg::ref_ptr<osg::Texture2D>(), true, false);
    }

    MyGUI::ITexture& GlMapOverlay::getTexture()
    {
        return *mGuiTexture;
    }

    bool GlMapOverlay::copyResult(osg::Camera* camera, unsigned int frame)
    {
        ImageDestMap::iterator it = mPendingImageDest.find(camera);
        if (it == mPendingImageDest.end())
            return true;
        else
        {
            ImageDest& imageDest = it->second;
            if (imageDest.mFrameDone == 0)
                imageDest.mFrameDone
                    = frame + 2; // wait an extra frame to ensure the draw thread has completed its frame.
            if (imageDest.mFrameDone > frame)
            {
                ++it;
                return false;
            }

            mOverlayImage->copySubImage(imageDest.mX, imageDest.mY, 0, imageDest.mImage);
            mPendingImageDest.erase(it);
            return true;
        }
    }

    void GlMapOverlay::markForRemoval(osg::Camera* camera)
    {
        CameraVector::iterator found = std::find(mActiveCameras.begin(), mActiveCameras.end(), camera);
        if (found == mActiveCameras.end())
        {
            Log(Debug::Error) << "Error: GlobalMap trying to remove an inactive camera";
            return;
        }
        mActiveCameras.erase(found);
        mCamerasPendingRemoval.push_back(camera);
    }

    void GlMapOverlay::cleanupCameras()
    {
        for (auto& camera : mCamerasPendingRemoval)
            removeCamera(camera);

        mCamerasPendingRemoval.clear();
    }

    void GlMapOverlay::removeCamera(osg::Camera* cam)
    {
        cam->removeChildren(0, cam->getNumChildren());
        mRoot->removeChild(cam);
    }
}

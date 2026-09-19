#ifndef GAME_RENDER_GLMAPOVERLAY_H
#define GAME_RENDER_GLMAPOVERLAY_H

#include <map>
#include <memory>
#include <vector>

#include <osg/ref_ptr>

#include "mapoverlay.hpp"

namespace osg
{
    class Camera;
    class Group;
    class Image;
    class Texture2D;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MyGUIPlatform
{
    class OSGTexture;
}

namespace MWRender
{
    class GlRenderer;

    /// The world map's overlay as the rasterizer draws it: upstream's `GlobalMap` render-to-texture
    /// path, moved here whole. A paint is a camera hung under the renderer's root that blits a
    /// textured quad into the overlay texture on the next frame, through the `globalmap` shader and
    /// the land alpha; a camera that has drawn is taken down between traversals, and where the
    /// paint is wanted in main memory too an image is attached to the camera and copied into the
    /// overlay image two frames later.
    class GlMapOverlay final : public MapOverlay
    {
    public:
        /// @param root where the cameras hang: the renderer's traversal root, which upstream's
        ///        `GlobalMap` was handed as the local map's.
        /// @param renderer which is told this exists, so it can take the drawn cameras down once a
        ///        frame, as upstream's `WindowManager::onFrame` did.
        GlMapOverlay(
            const MapOverlaySpec& spec, osg::Group& root, Resource::ResourceSystem& resources, GlRenderer& renderer);
        ~GlMapOverlay() override;

        void paintTile(const SceneUtil::ImageRegion& destination, std::shared_ptr<OffscreenView> tile) override;
        void paintImage(const SceneUtil::ImageRegion& destination, osg::ref_ptr<osg::Image> image,
            const SceneUtil::ImageRegion& source) override;
        void replace(osg::ref_ptr<osg::Image> image) override;
        void clear() override;
        const osg::Image& getImage() const override { return *mOverlayImage; }
        MyGUI::ITexture& getTexture() override;

        /*internal:*/
        /// Removes cameras that have already been rendered. Called every frame by the renderer to
        /// ensure that we do not render the same map more than once. Note, this cleanup is difficult
        /// to implement in an automated fashion, since we can't alter the scene graph structure from
        /// within an update callback.
        void cleanupCameras();

        bool copyResult(osg::Camera* cam, unsigned int frame);

        /// Mark a camera for cleanup in the next update. For internal use only.
        void markForRemoval(osg::Camera* camera);

    private:
        /// Request rendering a 2d quad onto mOverlayTexture.
        /// x, y, width and height are the destination coordinates (top-left coordinate origin)
        /// @param cpuCopy copy the resulting render onto mOverlayImage as well?
        void requestOverlayTextureUpdate(int x, int y, int width, int height, osg::ref_ptr<osg::Texture2D> texture,
            bool clear, bool cpuCopy, float srcLeft = 0.f, float srcTop = 0.f, float srcRight = 1.f,
            float srcBottom = 1.f);

        void removeCamera(osg::Camera* cam);

        GlRenderer& mRenderer;
        Resource::ResourceSystem& mResources;

        int mWidth;
        int mHeight;

        osg::ref_ptr<osg::Group> mRoot;

        typedef std::vector<osg::ref_ptr<osg::Camera>> CameraVector;
        CameraVector mActiveCameras;

        CameraVector mCamerasPendingRemoval;

        struct ImageDest
        {
            ImageDest()
                : mX(0)
                , mY(0)
                , mFrameDone(0)
            {
            }

            osg::ref_ptr<osg::Image> mImage;
            int mX, mY;
            unsigned int mFrameDone;
        };

        typedef std::map<osg::ref_ptr<osg::Camera>, ImageDest> ImageDestMap;

        ImageDestMap mPendingImageDest;

        osg::ref_ptr<osg::Texture2D> mAlphaTexture;

        // GPU copy of overlay
        // Note, uploads are pushed through a Camera, instead of through mOverlayImage
        osg::ref_ptr<osg::Texture2D> mOverlayTexture;

        // CPU copy of overlay
        osg::ref_ptr<osg::Image> mOverlayImage;

        std::unique_ptr<MyGUIPlatform::OSGTexture> mGuiTexture;
    };
}

#endif

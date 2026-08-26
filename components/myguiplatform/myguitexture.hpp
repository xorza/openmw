#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUITEXTURE_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUITEXTURE_H

#include <cstdint>
#include <span>

#include <MyGUI_ITexture.h>

#include <osg/ref_ptr>

#include "regiontexture.hpp"

namespace osg
{
    class Image;
    class Texture2D;
    class StateSet;
}

namespace Resource
{
    class ImageManager;
}

namespace MyGUIPlatform
{

    /// A GUI texture drawn by OpenSceneGraph.
    ///
    /// **The picture is allocated once and written where it lies.** MyGUI hands out a buffer to fill
    /// and takes it back filled; the buffer is this texture's own image, so a picture written every
    /// frame — a video — costs a `memcpy` and the rows it changed, rather than an image, a texture
    /// object and the whole surface.
    ///
    /// **What makes that safe is one promotion.** The draw traversal may run beside the update
    /// traversal of the next frame (`RenderManager::Drawable::drawImplementation`), so a picture that
    /// has been drawn with cannot simply be overwritten. Until it has been, nothing can be reading
    /// it. The first write *after* it has been copies the image once and marks the copy
    /// `osg::Object::DYNAMIC` — which `RenderManager::doRender` passes on to the drawable, and which
    /// is what makes `osgViewer` hold the next update until this frame's draw is done. Every write
    /// after that is in place, for the life of the texture.
    class OSGTexture final : public MyGUI::ITexture, public RegionTexture
    {
    public:
        /// Which rows of the picture have been written and not yet sent.
        ///
        /// **Rows and not a rectangle**, because a run of whole rows is one contiguous span of both
        /// the image and the upload, and sending one needs no pixel-store state to describe the gaps.
        /// The caller that writes part of a picture is the world map writing a cell-sized block of
        /// it, and a block is a few rows either way.
        struct DirtyRows
        {
            int mFirst = 0;
            int mCount = 0;

            bool empty() const { return mCount == 0; }

            /// Widens this to cover `count` rows from `first` as well.
            void add(int first, int count);

            void clear() { *this = DirtyRows{}; }
        };

        OSGTexture(const std::string& name, Resource::ImageManager* imageManager);
        OSGTexture(osg::Texture2D* texture, osg::StateSet* injectState = nullptr);
        ~OSGTexture() override;

        osg::StateSet* getInjectState() { return mInjectState.get(); }

        const std::string& getName() const override { return mName; }

        void createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format) override;
        void loadFromFile(const std::string& fname) override;
        void saveToFile(const std::string& fname) override;

        void destroy() override;

        void* lock(MyGUI::TextureUsage access) override;
        void unlock() override;
        bool isLocked() const override { return mLocked; }

        int getWidth() const override { return mWidth; }
        int getHeight() const override { return mHeight; }

        MyGUI::PixelFormat getFormat() const override { return mFormat; }
        MyGUI::TextureUsage getUsage() const override { return mUsage; }
        size_t getNumElemBytes() const override { return mNumElemBytes; }

        MyGUI::IRenderTarget* getRenderTarget() override;

        void setShader(const std::string& shaderName) override;

        /// **What MyGUI's own interface cannot ask for.** The picture is on this side anyway, so
        /// writing part of it and sending the rows it touched is the whole of it — the world map
        /// paints eighteen pixels square when a cell arrives and used to send the surface.
        void writeRegion(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
            std::span<const std::uint8_t> rows) override;

        /*internal:*/

        osg::Texture2D* getTexture() const { return mTexture.get(); }

        /// Says that the interface has handed this texture to the draw traversal.
        ///
        /// **Not a frame count and never reset.** What it decides is whether a write may land in an
        /// image something might be reading, and a texture that has been batched once is one whose
        /// image the draw thread has seen.
        void markDrawn() { mDrawn = true; }

        /// What the next apply will send, for a test that cannot open a GL context.
        DirtyRows getDirtyRows() const;

        /// Whether the picture has been copied and marked dynamic. False until the first write after
        /// the first draw, and true for ever after.
        bool isDynamic() const { return mDynamic; }

    private:
        /// The subload callback, which holds the rows and does the upload.
        ///
        /// **Defined where the OSG texture headers are and reached through the texture**, so that
        /// this header carries neither them nor a member of an incomplete type — `RenderManager`
        /// keeps its textures by value in a map, so every part of this class it can see has to be
        /// complete where it can see it.
        class Subload;

        /// The callback on `mTexture`, or null where the texture is not one `createManual` built.
        Subload* getSubload() const;

        /// Builds `mTexture` and its callback over `mImage`, in the shape `createManual` asked for.
        void makeTexture();

        /// Copies the picture and starts drawing from the copy, marked dynamic.
        ///
        /// Called before a write, and only where the texture has been drawn with and is not dynamic
        /// yet — so at most once in its life. The old image and the old texture stay alive as long as
        /// the batch that named them does, which is what lets the draw finish reading them.
        void promote();

        std::string mName;
        Resource::ImageManager* mImageManager;

        osg::ref_ptr<osg::Image> mImage;
        osg::ref_ptr<osg::Texture2D> mTexture;
        osg::ref_ptr<osg::StateSet> mInjectState;

        MyGUI::PixelFormat mFormat;
        MyGUI::TextureUsage mUsage;
        size_t mNumElemBytes;

        int mWidth;
        int mHeight;

        bool mLocked = false;
        bool mDrawn = false;
        bool mDynamic = false;
    };

}

#endif

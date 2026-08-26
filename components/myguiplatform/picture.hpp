#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_PICTURE_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_PICTURE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <MyGUI_RenderFormat.h>

namespace MyGUI
{
    class ITexture;
}

namespace osg
{
    class Image;
}

namespace MyGUIPlatform
{
    class RegionTexture;

    /// Pixels the game holds in main memory, in front of the GUI.
    ///
    /// **Renderer-neutral, despite where it lives.** It names MyGUI's own factory and its
    /// `lock`/`unlock`, which every backend implements and none of which says what draws. It is here
    /// because this is where OSG meets MyGUI, and an `osg::Image` is what the game's video decoder,
    /// savegames, fog of war and map both produce and keep.
    class Picture
    {
    public:
        /// @param label what the texture is called for anything that lists them. Made unique here,
        ///        because MyGUI keys its textures by name and two pictures are two textures.
        explicit Picture(std::string_view label);
        ~Picture();

        Picture(const Picture&) = delete;
        Picture& operator=(const Picture&) = delete;

        /// Movable, because a picture is often a member of something a container holds.
        Picture(Picture&& other) noexcept;
        Picture& operator=(Picture&& other) noexcept;

        /// Copies the whole image in, making the texture the first time and again whenever the image
        /// changes shape. All or nothing: MyGUI hands out a fresh buffer on every lock and there is
        /// no asking it for part of one.
        void set(const osg::Image& image);

        /// Copies a rectangle of `image` in, where the backend can take one.
        ///
        /// **Falls back to the whole image where it cannot**, which is what makes this safe to call
        /// from anywhere: `RegionTexture` is an offer a backend makes rather than one it owes, and a
        /// caller that had to ask would end up with two code paths of its own. Both of this fork's
        /// backends do make it, so what the fallback is left covering is a picture MyGUI took at
        /// three channels.
        ///
        /// `image` is the whole picture and the rectangle names part of it, so the rows are gathered
        /// out of it here. The texture must already exist — a `set` comes first — and the rectangle
        /// must lie inside it.
        void setRegion(const osg::Image& image, int x, int y, int width, int height);

        /// Null until the first `set`.
        MyGUI::ITexture* getTexture() const { return mTexture; }

    private:
        /// Gives the texture back where there is still an interface to give it back to.
        void release();

        /// The rectangle's rows, gathered out of the image so the backend gets them tightly packed.
        /// Kept because a picture written in part is written in part again and again.
        std::vector<std::uint8_t> mRegionScratch;

        std::string mName;
        MyGUI::ITexture* mTexture = nullptr;

        /// The same texture, where it can take a rectangle. Asked once when the texture is made
        /// rather than on every write, because a picture written in part is written in part again
        /// and again and a `dynamic_cast` is not free.
        RegionTexture* mRegion = nullptr;
        int mWidth = 0;
        int mHeight = 0;
        MyGUI::PixelFormat mFormat = MyGUI::PixelFormat::Unknow;
    };

}

#endif

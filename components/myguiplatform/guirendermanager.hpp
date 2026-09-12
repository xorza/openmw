#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_GUIRENDERMANAGER_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_GUIRENDERMANAGER_H

#include <memory>
#include <string>
#include <string_view>

#include <MyGUI_RenderManager.h>

namespace osg
{
    class Texture2D;
}

namespace MyGUIPlatform
{

    /// MyGUI's render manager, plus the calls MyGUI does not declare and every backend needs.
    ///
    /// **Neutral, despite where it lives**: this is MyGUI's own interface with two lifetime hooks
    /// on it, and it has no idea what draws. It exists so that one
    /// `Platform` serves every backend — the log and the data manager beside it are the same either
    /// way, and it is only the render manager that is anybody's.
    class GuiRenderManager : public MyGUI::RenderManager
    {
    public:
        /// Called once, after MyGUI's log manager exists, because this logs.
        virtual void initialise() = 0;

        /// Called while whatever the backend attached itself to is still alive, which is why it is
        /// not the destructor.
        virtual void shutdown() = 0;

        /// Whether what is drawn from now on is added to what is under it rather than blended over
        /// it. `AdditiveLayer` turns it on around the one layer that wants it and off again.
        ///
        /// **Here rather than on the layer**, because the layer is handed an `IRenderTarget` and a
        /// scaled layer hands it a proxy standing in front of the real one; the blend mode belongs
        /// to whatever is finally drawing, which is this.
        virtual void setAdditiveBlend(bool additive) = 0;

        /// A picture the game holds as an `osg::Texture2D` over an `osg::Image`, drawn from there.
        ///
        /// **The one route from a picture in main memory to the interface.** The fog of war, the
        /// world map, a save's thumbnail and a video frame are all `osg::Image`s the game writes and
        /// then marks dirty, which is how upstream hands them to the rasterizer: it draws the texture
        /// as it stands. A backend that never opens a GL context mirrors the image instead, and reads
        /// it again whenever `osg::Image::getModifiedCount` says it changed — so a caller writes the
        /// image the way it always did and never asks which backend it got.
        ///
        /// The texture and its image belong to the caller and outlive what comes back.
        virtual std::unique_ptr<MyGUI::ITexture> shareTexture(osg::Texture2D& texture) = 0;
    };

    /// `shareTexture` on whichever render manager is up, for the callers that have no handle to it.
    inline std::unique_ptr<MyGUI::ITexture> shareTexture(osg::Texture2D& texture)
    {
        return static_cast<GuiRenderManager&>(MyGUI::RenderManager::getInstance()).shareTexture(texture);
    }

    /// A name nothing else in MyGUI's texture table will have, out of `label`.
    ///
    /// **MyGUI keys its textures by name**, so everything that makes one needs a name of its own —
    /// and one counter for all of them, because two makers counting separately hand out the same
    /// name the moment they are given the same label.
    inline std::string uniqueTextureName(std::string_view label)
    {
        static unsigned int next = 0;
        return std::string(label) + " " + std::to_string(next++);
    }

}

#endif

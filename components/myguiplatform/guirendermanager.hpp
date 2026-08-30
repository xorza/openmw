#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_GUIRENDERMANAGER_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_GUIRENDERMANAGER_H

#include <memory>

#include <MyGUI_RenderManager.h>

namespace osg
{
    class Texture2D;
}

namespace MyGUIPlatform
{

    /// MyGUI's render manager, plus the calls MyGUI does not declare and every backend needs.
    ///
    /// **Neutral, despite where it lives**, for the reason `Picture` is: this is MyGUI's own
    /// interface with two lifetime hooks on it, and it has no idea what draws. It exists so that one
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

        /// A picture the game already holds on the device, drawn where it lies. Null where this
        /// backend cannot draw one, and the caller then hands over the pixels instead.
        ///
        /// **The video is the one caller.** Its decoder writes into an OSG texture and swaps the
        /// image under it every frame, which the rasterizer draws untouched; a backend that never
        /// opens a GL context cannot see that texture at all and is given the frame through
        /// `Picture` instead.
        virtual std::unique_ptr<MyGUI::ITexture> shareTexture(osg::Texture2D& texture) = 0;
    };

}

#endif

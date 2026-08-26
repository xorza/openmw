#include "additivelayer.hpp"

#include "guirendermanager.hpp"

namespace MyGUIPlatform
{

    void AdditiveLayer::renderToTarget(MyGUI::IRenderTarget* target, bool update)
    {
        // **The manager, not the target.** A scaled layer is drawn through a proxy target that
        // only adjusts the pixel scale, and the blend mode is not its to answer. Every backend's
        // manager derives from `GuiRenderManager`, so this cast is the one that holds.
        GuiRenderManager& renderManager = static_cast<GuiRenderManager&>(MyGUI::RenderManager::getInstance());

        renderManager.setAdditiveBlend(true);

        MyGUI::OverlappedLayer::renderToTarget(target, update);

        renderManager.setAdditiveBlend(false);
    }

}

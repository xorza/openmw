#include "additivelayer.hpp"

#include "guirendermanager.hpp"

namespace MyGUIPlatform
{

    void AdditiveLayer::renderToTarget(MyGUI::IRenderTarget* target, bool update)
    {
        // The manager rather than the target, which for a scaled layer is a proxy that only adjusts the pixel scale
        GuiRenderManager& renderManager = static_cast<GuiRenderManager&>(MyGUI::RenderManager::getInstance());

        renderManager.setAdditiveBlend(true);

        MyGUI::OverlappedLayer::renderToTarget(target, update);

        renderManager.setAdditiveBlend(false);
    }

}

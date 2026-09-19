#include "additivelayer.hpp"

#include <MyGUI_RenderManager.h>

#include "rendermanager.hpp"

namespace MyGUIRtx
{
    void AdditiveLayer::renderToTarget(MyGUI::IRenderTarget* target, bool update)
    {
        // The manager rather than the target, which for a scaled layer is a proxy that only adjusts
        // the pixel scale.
        RenderManager& renderManager = static_cast<RenderManager&>(MyGUI::RenderManager::getInstance());

        renderManager.setAdditiveBlend(true);

        MyGUI::OverlappedLayer::renderToTarget(target, update);

        renderManager.setAdditiveBlend(false);
    }
}

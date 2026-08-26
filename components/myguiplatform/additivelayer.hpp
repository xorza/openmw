#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_ADDITIVELAYER
#define OPENMW_COMPONENTS_MYGUIPLATFORM_ADDITIVELAYER

#include <MyGUI_OverlappedLayer.h>

namespace MyGUIPlatform
{

    /// @brief A Layer rendering with additive blend mode.
    ///
    /// **It knows no backend.** What additive means is the render manager's answer, and there is
    /// more than one of those; this only says when.
    class AdditiveLayer final : public MyGUI::OverlappedLayer
    {
    public:
        MYGUI_RTTI_DERIVED(AdditiveLayer)

        void renderToTarget(MyGUI::IRenderTarget* target, bool update) override;
    };

}

#endif

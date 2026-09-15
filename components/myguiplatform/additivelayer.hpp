#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_ADDITIVELAYER
#define OPENMW_COMPONENTS_MYGUIPLATFORM_ADDITIVELAYER

#include <MyGUI_OverlappedLayer.h>

namespace MyGUIPlatform
{

    /// @brief A Layer rendering with additive blend mode.
    class AdditiveLayer final : public MyGUI::OverlappedLayer
    {
    public:
        MYGUI_RTTI_DERIVED(AdditiveLayer)

        void renderToTarget(MyGUI::IRenderTarget* target, bool update) override;
    };

}

#endif

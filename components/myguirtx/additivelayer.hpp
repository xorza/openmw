#pragma once

#include <MyGUI_OverlappedLayer.h>

namespace MyGUIRtx
{
    /// The layer the hit overlay is drawn on, added to what is under it rather than blended over
    /// it: upstream's `MyGUIPlatform::AdditiveLayer` for this backend, which has no state set to
    /// inject and marks the batches it gathers instead. Registered under the same type name, so the
    /// layout that names an `AdditiveLayer` names this one here.
    class AdditiveLayer final : public MyGUI::OverlappedLayer
    {
    public:
        MYGUI_RTTI_DERIVED(AdditiveLayer)

        void renderToTarget(MyGUI::IRenderTarget* target, bool update) override;
    };
}

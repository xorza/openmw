#pragma once

#include "slot.hpp"

namespace Rtx
{
    class Renderer;

    /// A scene slot of a renderer's own — `Renderer::addViewScene` — given back when this goes.
    /// What a picture inside the interface holds instead of an index it has to remember to drop:
    /// a constructor that throws after the slot was taken unwinds it, and nothing can drop it
    /// twice. Move-only, because the slot is one thing; one holding nothing is what a moved-from
    /// one and a default one are, and `get` is not asked of either.
    class ViewScene
    {
    public:
        ViewScene() = default;
        explicit ViewScene(Renderer& renderer);
        ~ViewScene();

        ViewScene(const ViewScene&) = delete;
        ViewScene& operator=(const ViewScene&) = delete;

        ViewScene(ViewScene&& other) noexcept;
        ViewScene& operator=(ViewScene&& other) noexcept;

        bool holds() const { return mRenderer != nullptr; }

        SceneSlot get() const;

    private:
        void drop();

        /// Null where this holds no slot.
        Renderer* mRenderer = nullptr;
        SceneSlot mSlot;
    };
}

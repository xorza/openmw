#include "viewscene.hpp"

#include <cassert>
#include <utility>

#include "renderer.hpp"

namespace Rtx
{
    ViewScene::ViewScene(Renderer& renderer)
        : mRenderer(&renderer)
        , mSlot(renderer.addViewScene())
    {
    }

    ViewScene::~ViewScene()
    {
        drop();
    }

    ViewScene::ViewScene(ViewScene&& other) noexcept
        : mRenderer(std::exchange(other.mRenderer, nullptr))
        , mSlot(other.mSlot)
    {
    }

    ViewScene& ViewScene::operator=(ViewScene&& other) noexcept
    {
        if (this != &other)
        {
            drop();
            mRenderer = std::exchange(other.mRenderer, nullptr);
            mSlot = other.mSlot;
        }

        return *this;
    }

    SceneSlot ViewScene::get() const
    {
        assert(holds() && "the slot of a view scene that holds none");
        return mSlot;
    }

    void ViewScene::drop()
    {
        if (mRenderer != nullptr)
            mRenderer->dropViewScene(mSlot);
        mRenderer = nullptr;
    }
}

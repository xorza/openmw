#include "sceneslots.hpp"

#include <cassert>
#include <cstdint>
#include <utility>

#include <components/rtx/runs.hpp>

#include "device.hpp"
#include "graveyard.hpp"

namespace Rtx
{
    SceneSlots::SceneSlots(const Device& device)
        : mDevice(device)
    {
    }

    SceneSlot SceneSlots::add()
    {
        if (const Index taken = mFreeViews.take(); taken != sNoIndex)
            return SceneSlot::view(taken);

        mViews.push_back(nullptr);
        return SceneSlot::view(static_cast<std::uint32_t>(mViews.size() - 1));
    }

    void SceneSlots::drop(const SceneSlot slot)
    {
        assert(!slot.isWorld() && "the world's slot given back");
        assert(slot.getViewIndex() < mViews.size() && "a scene slot nothing handed out");
        assert(!mFreeViews.isFree(slot.getViewIndex()) && "a scene given back twice");

        bury(slot);
        mFreeViews.free(slot.getViewIndex());
    }

    void SceneSlots::bury(const SceneSlot slot)
    {
        std::unique_ptr<DeviceScene>& held = slotAt(slot);
        if (held != nullptr)
            mDevice.getGraveyard().bury(std::shared_ptr<void>(std::move(held)));
    }

    DeviceScene& SceneSlots::hold(const SceneSlot slot, std::unique_ptr<DeviceScene> scene)
    {
        std::unique_ptr<DeviceScene>& held = slotAt(slot);
        assert(held == nullptr && "a scene put over another");
        held = std::move(scene);
        return *held;
    }

    const std::unique_ptr<DeviceScene>& SceneSlots::slotAt(const SceneSlot slot) const
    {
        if (slot.isWorld())
            return mWorld;

        assert(slot.getViewIndex() < mViews.size() && "a scene slot nothing handed out");
        return mViews[slot.getViewIndex()];
    }

    std::unique_ptr<DeviceScene>& SceneSlots::slotAt(const SceneSlot slot)
    {
        return const_cast<std::unique_ptr<DeviceScene>&>(std::as_const(*this).slotAt(slot));
    }

    const DeviceScene* SceneSlots::find(const SceneSlot slot) const
    {
        return slotAt(slot).get();
    }

    DeviceScene* SceneSlots::find(const SceneSlot slot)
    {
        return slotAt(slot).get();
    }

    const DeviceScene& SceneSlots::at(const SceneSlot slot) const
    {
        const DeviceScene* held = find(slot);
        assert(held != nullptr && "a scene slot nothing holds");
        return *held;
    }

    DeviceScene& SceneSlots::at(const SceneSlot slot)
    {
        return const_cast<DeviceScene&>(std::as_const(*this).at(slot));
    }
}

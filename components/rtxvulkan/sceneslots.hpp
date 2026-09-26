#pragma once

#include <memory>
#include <vector>

#include <components/rtx/slot.hpp>
#include <components/rtx/slots.hpp>

#include "devicescene.hpp"

namespace Rtx
{
    class Device;

    /// The scenes a renderer holds, by slot: the world's, and one for each picture inside the
    /// interface — null until each is given a scene — with the slots nothing holds.
    class SceneSlots
    {
    public:
        explicit SceneSlots(const Device& device);

        /// A slot for a picture's scene, empty until something is put into it.
        SceneSlot add();

        /// Lets the slot go, and what it holds with it, as `bury` does.
        void drop(SceneSlot slot);

        /// Lets go of what `slot` holds, under the next submit and no sooner: a picture of it
        /// recorded and not yet carried rides that submit, and so does the last placement's refit.
        /// The graveyard frees a scene after every structure, because a structure the scene retired
        /// gives its room back to a storage the scene owns.
        void bury(SceneSlot slot);

        /// Puts `scene` into `slot`, which holds nothing.
        DeviceScene& hold(SceneSlot slot, std::unique_ptr<DeviceScene> scene);

        /// What `slot` holds, or null. A slot nothing handed out is a caller bug, and asserted.
        const DeviceScene* find(SceneSlot slot) const;
        DeviceScene* find(SceneSlot slot);

        /// What `slot` holds, which something must: asked of an empty slot is a caller bug, and
        /// asserted.
        const DeviceScene& at(SceneSlot slot) const;
        DeviceScene& at(SceneSlot slot);

    private:
        const std::unique_ptr<DeviceScene>& slotAt(SceneSlot slot) const;
        std::unique_ptr<DeviceScene>& slotAt(SceneSlot slot);

        const Device& mDevice;
        std::unique_ptr<DeviceScene> mWorld;
        std::vector<std::unique_ptr<DeviceScene>> mViews;
        SlotPool mFreeViews;
    };
}

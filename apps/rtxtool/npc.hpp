#pragma once

#include <string_view>

namespace ESM
{
    struct NPC;
}

namespace RtxTool
{
    struct ActorModel;
    class World;

    /// The person `id` names, or null where the content files have no such record.
    ///
    /// Matched the way Morrowind matches a record id: case-insensitively, so `fargoth` and `Fargoth`
    /// are one person.
    const ESM::NPC* findNpc(const World& world, std::string_view id);

    /// Assembles a person out of the body parts their race and sex call for.
    ///
    /// **An NPC record contains almost nothing about how they look.** It names a race, a sex, a head
    /// and a hair, and the rest is a lookup: one `BODY` record per limb for that race, each attached
    /// to a bone of a shared skeleton. Morrowind draws a person the way it draws a paper doll, and
    /// that is why an NPC could not simply be loaded from a file the way a creature can.
    ///
    /// What they have on comes out of the record's own inventory list, which is what the game equips
    /// the first time it builds one. A garment claims the body slots it covers and hides the ones it
    /// merely reaches over, so a robe leaves no shins showing and a cuirass leaves no chest. A shield
    /// is a garment like any other — every one the game ships is authored as a body part — and a
    /// weapon is not: it is the item's own model put in a hand, and the person is posed in the idle
    /// that goes with it.
    ///
    /// **The weapon is drawn, which the game would not do.** Morrowind keeps an undrawn weapon out
    /// of sight and only puts one in a hand when its owner means to use it; a harness that hid it
    /// would be hiding the thing it exists to look at.
    ///
    /// @param dressed false to leave them in their skin and empty-handed, which is what a renderer
    ///        wants to look at: skin is the hardest surface in the game to get right and the one the
    ///        shipped textures have the most light painted into.
    ActorModel buildNpc(World& world, const ESM::NPC& npc, bool dressed = true);
}

#pragma once

#include <span>
#include <vector>

#include <osg/Vec3f>

#include <components/rtx/ripple.hpp>

#include "../../mwworld/ptr.hpp"
#include "../sceneframe.hpp"

namespace MWWorld
{
    class CellStore;
}

namespace MWRender
{
    /// What disturbs the water, decided as the rasterizer's `RippleSimulation` decides it: every
    /// actor the scene adds is an emitter, and one that stands in water without being submerged —
    /// or walks on it — presses a ring under its feet every frame; a strike on the water presses
    /// one where it landed. What comes out is the frame's list of `Rtx::RippleImpulse`, which the
    /// trace's ripple field takes.
    ///
    /// **A copy of `RippleSimulation::update`'s rule, and named as one.** That rule asks
    /// `MWBase::World` whether an actor is in water and cannot be lifted to `components/`, and
    /// lifting it out of `ripplesimulation.cpp` would be an edit to upstream's file.
    class RippleEmitters
    {
    public:
        void add(const MWWorld::Ptr& ptr);
        void remove(const MWWorld::Ptr& ptr);

        /// Drops every emitter that stands in `cell` but the player, who is refreshed by pointer.
        void removeCell(const MWWorld::CellStore& cell);

        /// Something struck the water at `at`, which is pressed on the next `update` if it landed
        /// within `sStrikeReach` of the surface — `RippleSimulation::emitRipple`'s own test.
        void splash(const osg::Vec3f& at);

        /// Decides this frame's impulses: the wading emitters and the strikes since the last.
        /// @param water where the surface stands, which a strike is tested against, and whether
        ///        there is one.
        void update(const WaterState& water);

        /// What `update` decided, until the next.
        std::span<const Rtx::RippleImpulse> getImpulses() const { return mImpulses; }

        /// Everything, for a world that is going.
        void clear();

    private:
        /// The ring a footfall or a strike presses, in world units: `RippleSimulation::emitRipple`'s
        /// `particleRippleSizeInUnits`.
        static constexpr float sFootfall = 12.0f;

        /// How far above or below the surface a strike still presses a ring, in world units:
        /// `RippleSimulation::emitRipple`'s own twenty.
        static constexpr float sStrikeReach = 20.0f;

        std::vector<MWWorld::ConstPtr> mEmitters;

        /// Strikes since the last update, and the impulses the last update decided. Both refilled
        /// per frame and never freed.
        std::vector<osg::Vec3f> mStrikes;
        std::vector<Rtx::RippleImpulse> mImpulses;
    };
}

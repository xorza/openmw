#include "rippleemitters.hpp"

#include <algorithm>

#include <osg/Vec2f>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/class.hpp"
#include "../ripplerules.hpp"

namespace MWRender
{
    void RippleEmitters::add(const MWWorld::Ptr& ptr)
    {
        if (std::find(mEmitters.begin(), mEmitters.end(), ptr) == mEmitters.end())
            mEmitters.push_back(ptr);
    }

    void RippleEmitters::remove(const MWWorld::Ptr& ptr)
    {
        std::erase(mEmitters, MWWorld::ConstPtr(ptr));
    }

    void RippleEmitters::removeCell(const MWWorld::CellStore& cell)
    {
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        std::erase_if(mEmitters,
            [&](const MWWorld::ConstPtr& ptr) { return ptr.isInCell() && ptr.getCell() == &cell && ptr != player; });
    }

    void RippleEmitters::splash(const osg::Vec3f& at)
    {
        mStrikes.push_back(at);
    }

    void RippleEmitters::update(const WaterState& water)
    {
        mImpulses.clear();

        if (!water.isShown())
        {
            mStrikes.clear();
            return;
        }

        const MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();

        for (MWWorld::ConstPtr& ptr : mEmitters)
        {
            // The player's pointer is fetched afresh, to follow a cell change; every other actor
            // is removed and added again around one by the scene.
            if (ptr == player)
                ptr = player;

            if (!ptr.isInCell())
                continue;

            if (!isWading(world, ptr))
                continue;

            // Every frame and not on a timer, as the rasterizer's own field is pressed: a ring a
            // frame is a wake, and one every second and a half is a row of rings.
            const osg::Vec3f at = ptr.getRefData().getPosition().asVec3();
            mImpulses.push_back(Rtx::RippleImpulse{ .mAt = osg::Vec2f(at.x(), at.y()), .mSize = sRippleSize });
        }

        for (const osg::Vec3f& strike : mStrikes)
            if (strikesWater(strike.z(), water.mHeight))
                mImpulses.push_back(
                    Rtx::RippleImpulse{ .mAt = osg::Vec2f(strike.x(), strike.y()), .mSize = sRippleSize });

        mStrikes.clear();
    }

    void RippleEmitters::clear()
    {
        mEmitters.clear();
        mStrikes.clear();
        mImpulses.clear();
    }
}

#include "rippleemitters.hpp"

#include <algorithm>
#include <cmath>

#include <osg/Vec2f>

#include "../../mwbase/environment.hpp"
#include "../../mwbase/world.hpp"
#include "../../mwworld/cellstore.hpp"
#include "../../mwworld/class.hpp"

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

    void RippleEmitters::update(const bool waterEnabled, const float waterHeight)
    {
        mImpulses.clear();

        if (!waterEnabled)
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

            const osg::Vec3f at = ptr.getRefData().getPosition().asVec3();
            const bool wading
                = (world.isUnderwater(ptr.getCell(), at) && !world.isSubmerged(ptr)) || world.isWalkingOnWater(ptr);
            if (!wading)
                continue;

            // Every frame and not on a timer, as the rasterizer's own field is pressed: a ring a
            // frame is a wake, and one every second and a half is a row of rings.
            mImpulses.push_back(Rtx::RippleImpulse{ .mAt = osg::Vec2f(at.x(), at.y()), .mSize = sFootfall });
        }

        for (const osg::Vec3f& strike : mStrikes)
            if (std::abs(strike.z() - waterHeight) < 20.0f)
                mImpulses.push_back(
                    Rtx::RippleImpulse{ .mAt = osg::Vec2f(strike.x(), strike.y()), .mSize = sFootfall });

        mStrikes.clear();
    }

    void RippleEmitters::clear()
    {
        mEmitters.clear();
        mStrikes.clear();
        mImpulses.clear();
    }
}

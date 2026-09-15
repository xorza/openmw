#include "ground.hpp"

#include <components/terrain/world.hpp>

#include "groundcover.hpp"
#include "objectpaging.hpp"

namespace MWRender
{
    Ground::Ground() = default;
    Ground::Ground(Ground&& other) noexcept = default;
    Ground& Ground::operator=(Ground&& other) noexcept = default;
    Ground::~Ground() = default;
}

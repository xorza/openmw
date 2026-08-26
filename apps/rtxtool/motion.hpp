#pragma once

#include <cstdint>

namespace RtxTool
{
    /// Something that changes the scene between one traced frame and the next.
    ///
    /// **What makes a run of frames a run rather than the same frame over again.** A still world
    /// needs none, and passing none is what keeps `--repeat` measuring one frame's cost instead of a
    /// different frame each time; an actor needs one, because its vertices *are* the animation.
    class Motion
    {
    public:
        virtual ~Motion() = default;

        Motion(const Motion&) = delete;
        Motion& operator=(const Motion&) = delete;

        /// Advances to `frame` and re-walks whatever moved. False where nothing did, which is what
        /// spares the frame a placement it does not need.
        virtual bool step(std::uint32_t frame) = 0;

    protected:
        Motion() = default;
    };
}

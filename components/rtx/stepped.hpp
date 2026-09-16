#pragma once

#include <cassert>

namespace Rtx
{
    /// Which step of a fixed order an object stands at, asserted rather than written down — what
    /// `OwnedBy` is for threads, for time. The step is kept in both builds and checked only where
    /// asserts are on, so release pays for one byte and no comparison; an object that changed
    /// shape between builds is a worse trade than the byte.
    template <class Step>
    class Stepped
    {
    public:
        explicit Stepped(const Step at)
            : mAt(at)
        {
        }

        /// Moves to `next`, asserting this stands at one of `from`.
        template <class... From>
        void step(const Step next, const From... from)
        {
            expect(from...);
            mAt = next;
        }

        /// Asserts this stands at one of `any`.
        template <class... Any>
        void expect([[maybe_unused]] const Any... any) const
        {
            assert(((mAt == any) || ...) && "a call out of its turn");
        }

        Step get() const { return mAt; }

    private:
        Step mAt;
    };
}

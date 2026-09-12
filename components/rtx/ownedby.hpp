#pragma once

#include <cassert>
#include <thread>

namespace Rtx
{
    /// Which thread a member belongs to, asserted rather than written down.
    ///
    /// **Every class with a worker has members it calls "the frame thread's own".** That is a
    /// contract the code must keep, which is what an assert is for and not prose. A guard
    /// remembers the thread that built it, and a method that touches what it stands for asks it
    /// first.
    ///
    /// **The check is the assert and nothing else**, so release pays for no comparison. The id
    /// stays a member in both builds rather than hiding behind a gate: it is one word, and a class
    /// that changed shape between builds is a worse trade than that word.
    ///
    /// Untrusted data never reaches one of these. What it guards is which thread is calling, which
    /// is the code's own business and never the content's.
    class OwnedBy
    {
    public:
        /// Belongs to whoever built it, which for a member is whoever built the class.
        OwnedBy() = default;

        /// Fires where this is not that thread.
        void check() const { assert(mOwner == std::this_thread::get_id() && "a member touched from the wrong thread"); }

        /// Hands it to the calling thread, for the one case the design allows: an owner built on
        /// one thread and driven from another.
        void adopt() { mOwner = std::this_thread::get_id(); }

    private:
        std::thread::id mOwner = std::this_thread::get_id();
    };
}

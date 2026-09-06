#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <osg/Node>

namespace Rtx
{
    /// Which library a class was registered in, out of the ones this renderer's walks ask about.
    ///
    /// Everything else is `Other`. The list is short because it is what the casts down a walk are
    /// gated on, and nothing else is worth a name.
    enum class Library : std::uint8_t
    {
        Other,
        NifOsg,
        OsgParticle,
        SceneUtil,
        Terrain,
    };

    /// The library a node comes from, which is the one question cheap enough to ask of every node
    /// before a `dynamic_cast` that nearly all of them fail.
    ///
    /// A node from `osg` or `NifOsg` — which is nearly every node in a cell — answers this without
    /// the class hierarchy walk a failed cast does to say the same thing. Every traversal in this
    /// renderer runs over the whole graph, so the gate is what keeps a per-node question off the
    /// frame's cost.
    ///
    /// **Asked once per node and kept per class.** `libraryName()` is a virtual call returning a
    /// string literal, and a class has one: every instance of it hands back the same address, so
    /// which library that address names is settled by the first node of the class to ask. Two
    /// classes whose literals the linker merged carry the same string and so the same answer, which
    /// is what makes the memo exact rather than a guess about identity — the key is what a *library
    /// name* says, not which class said it.
    ///
    /// **Held by the walk that asks**, so the answers survive between frames rather than being
    /// worked out again per traversal. Nothing here is thread-safe and nothing asks it to be: a
    /// graph walk in this renderer runs on the thread that records the frame.
    class NodeLibrary
    {
    public:
        /// **The hit is here and the miss is not**, because the hit is what a walk pays per node: a
        /// scan of five pointers in one cache line, which a caller inlines, against a `.plt` call
        /// into `std::strcmp` that it cannot.
        Library of(const osg::Node& node) const
        {
            const char* const name = node.libraryName();

            for (std::size_t at = 0; at < mHeld; ++at)
                if (mNames[at] == name)
                    return mAnswers[at];

            return learn(name);
        }

    private:
        /// What a name met for the first time says, kept where there is room for it.
        Library learn(const char* name) const;

        /// How many library names are remembered. A cell's nodes come from five or six — `osg`,
        /// `NifOsg`, `SceneUtil`, `Terrain`, `osgParticle`, `MWRender` — so the table never fills,
        /// and a name that met a full one is answered without being kept rather than displacing one
        /// that is being asked about.
        static constexpr std::size_t sKept = 8;

        /// **`mutable` because this is a question and not a change.** The library a node comes from
        /// is the same before and after; these are workings.
        mutable std::array<const char*, sKept> mNames{};
        mutable std::array<Library, sKept> mAnswers{};
        mutable std::size_t mHeld = 0;
    };

    /// Whether `node` is exactly `type`, for the cast a library name cannot narrow.
    ///
    /// **The class where the library is every node in a cell.** `NodeLibrary` is the gate to reach
    /// for, because one library rules out whole kinds at once; it says nothing where the library is
    /// `osg` itself, which every plain group in a cell belongs to. This asks the narrower question
    /// at the same price, and it is sound only for a type nothing derives from — a subclass answers
    /// with its own name.
    inline bool isExactly(const osg::Node& node, const char* type)
    {
        return std::strcmp(node.className(), type) == 0;
    }
}

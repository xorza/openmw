#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <osg/Object>

namespace Rtx
{
    /// The classes the mirror treats specially. Everything else is walked as a plain node.
    enum class NodeKind : std::uint8_t
    {
        Other,
        Skeleton,
        LightSource,
        ParticleSystem,
        ParticleProcessor,
        ParticleUpdater,
        Sequence,
        RigGeometry,
        MorphGeometry,
    };

    /// Answers what kind a node is by its class, learning each class once. Keyed on the pair of
    /// literals `META_Object` answers with, so a node met again costs two pointer compares rather
    /// than the casts `learn` took; a subclass has a pair of its own and is learned as what it
    /// derives from, which is what makes the `static_cast` in `as` sound. The pointers are a
    /// cache key and not a claim: a second copy of a literal misses and is classified twice, never
    /// wrongly. One per walking thread, because the table is written on a miss.
    class NodeKinds
    {
    public:
        NodeKind of(const osg::Object& object) const;
        // Read by the tests and by nothing else.
        /// How many asks fell past the table. A content set with more classes than `sKept` shows
        /// up here rather than as a walk that silently went back to casting.
        std::uint32_t getOverflow() const { return mOverflow; }

    private:
        NodeKind learn(const osg::Object& object) const;

        struct Learned
        {
            const char* mLibrary = nullptr;
            const char* mClass = nullptr;
            NodeKind mKind = NodeKind::Other;
        };

        static constexpr std::size_t sKept = 32;

        mutable std::array<Learned, sKept> mLearned{};
        mutable std::size_t mHeld = 0;
        mutable std::uint32_t mOverflow = 0;
    };

    /// `object` as `T` where its kind is `wanted`, else null. A downcast the kind has already
    /// answered for, so it is static.
    template <class T, class Object>
    T* as(const NodeKind held, const NodeKind wanted, Object& object)
    {
        return held == wanted ? static_cast<T*>(&object) : nullptr;
    }
}

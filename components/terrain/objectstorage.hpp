#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <osg/Vec2i>
#include <osg/Vec3f>

#include <components/esm/refid.hpp>
#include <components/esm3/refnum.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/vfs/pathutil.hpp>

namespace Terrain
{
    /// One reference a chunk stands, reduced to what the paging needs of it.
    struct PagedCellRef
    {
        ESM::RefId mRefId{};
        ESM::RefNum mRefNum{};
        osg::Vec3f mPosition{};
        osg::Vec3f mRotation{};
        float mScale = 1.f;
    };

    /// Which references one walk of a cell's records collects, as a set of the two kinds.
    enum class RefKinds : unsigned int
    {
        /// What a chunk stands: the record types the paging draws, which is what makes a distant
        /// hillside look the same under both renderers.
        Paged = 1 << 0,

        /// What lights it and nothing stands: `LIGH`, which the paging never draws. The ray tracer
        /// lights the world with what it can reach rather than with what a camera can see, and a
        /// town four cells away that goes dark at dusk is the world stating something the content
        /// files do not.
        Lit = 1 << 1,

        Both = Paged | Lit,
    };

    constexpr bool holds(RefKinds set, RefKinds one)
    {
        return (static_cast<unsigned int>(set) & static_cast<unsigned int>(one)) != 0;
    }

    /// What a walk of the content files says of each reference, in the order the files stack,
    /// reduced the way they stack: a reference's last word wins, and a last word that deletes it
    /// leaves it out. Flat and sorted once, where a map makes a node per reference and is made again
    /// per cell; kept by a collector and emptied per call, so a walk of cell after cell allocates
    /// nothing once it has held the most any cell said.
    class RefStack
    {
    public:
        void assign(const ESM::RefNum& refNum, const PagedCellRef& ref)
        {
            mSaid.push_back(Said{ refNum, static_cast<std::uint32_t>(mSaid.size()), false, ref });
        }

        void erase(const ESM::RefNum& refNum)
        {
            mSaid.push_back(Said{ refNum, static_cast<std::uint32_t>(mSaid.size()), true, PagedCellRef{} });
        }

        void clear() { mSaid.clear(); }

        /// Every reference whose last word was not a deletion, that word, appended to `into` in
        /// reference order.
        void reduceInto(std::vector<PagedCellRef>& into)
        {
            std::sort(mSaid.begin(), mSaid.end(), [](const Said& a, const Said& b) {
                return a.mRefNum < b.mRefNum || (a.mRefNum == b.mRefNum && a.mOrder < b.mOrder);
            });

            for (std::size_t at = 0; at < mSaid.size(); ++at)
            {
                const Said& said = mSaid[at];
                const bool last = at + 1 == mSaid.size() || !(mSaid[at + 1].mRefNum == said.mRefNum);
                if (last && !said.mErased)
                    into.push_back(said.mRef);
            }
        }

    private:
        struct Said
        {
            ESM::RefNum mRefNum;

            /// Which word this was, so the last on a reference is the one kept.
            std::uint32_t mOrder;

            bool mErased;
            PagedCellRef mRef;
        };
        std::vector<Said> mSaid;
    };

    /// What one caller of `ObjectStorage::collect` keeps from one call to the next: whatever an
    /// implementation reads the content files with and reduces the references in. Made by
    /// `makeCollector` and handed back to every call that caller makes, so a thread walking cell
    /// after cell reuses its readers and its buffers and two threads never share them.
    class RefCollector
    {
    public:
        virtual ~RefCollector() = default;
    };

    /// What the paging and the ray tracer ask of the content files.
    ///
    /// **The seam `Terrain::Storage` already is, for the same reason.** The paging is a thousand
    /// lines of scene-graph work and about forty of reading records, and it was those forty that
    /// tied it to a running game. Behind this the two are the same code: a harness that stands a
    /// hillside up and the game that draws it cannot answer differently about what is on it.
    class ObjectStorage
    {
    public:
        virtual ~ObjectStorage() = default;

        /// Every reference of the `kinds` asked for in the square of `size` cells whose lowest
        /// corner is `startCell`, reduced by reference number the way the content files stack: a
        /// later file moving or deleting what an earlier one placed wins. Sorted by reference
        /// number. Both kinds come from one walk, because a walk opens the cell's readers and a
        /// second walk for the other kind opened them again; `getLight` says which kind a
        /// reference is.
        ///
        /// `into` is cleared first. Called from the paging's own working threads, so an
        /// implementation must be safe to call on several at once, each with a collector of its own.
        ///
        /// @param collector what `makeCollector` made for this caller.
        virtual void collect(float size, const osg::Vec2i& startCell, ESM::RefId worldspace, RefKinds kinds,
            RefCollector& collector, std::vector<PagedCellRef>& into) const = 0;

        /// A collector for one caller of `collect`, which that caller keeps.
        virtual std::unique_ptr<RefCollector> makeCollector() const = 0;

        /// What a `LIGH` record says its light is, or nothing where the id names no such record.
        ///
        /// **`SceneUtil::LightCommon` and not a shape of this fork's own**, because that is what
        /// `SceneUtil::createLightSource` takes and what every light the game places is built out
        /// of. A second reading of the same eight fields is a second answer waiting to drift.
        virtual std::optional<SceneUtil::LightCommon> getLight(const ESM::RefId& id) const = 0;

        /// The model a reference's record names, or empty where it names none — a marker, or a
        /// type that draws nothing.
        virtual VFS::Path::Normalized getModel(const ESM::RefId& id) const = 0;
    };
}

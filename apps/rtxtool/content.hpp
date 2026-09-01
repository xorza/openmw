#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include <osg/Matrixf>

#include <components/esm/refid.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esmloader/esmdata.hpp>
#include <components/files/collections.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/toutf8/toutf8.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "exteriorindex.hpp"
#include "objectstorage.hpp"

namespace boost::program_options
{
    class variables_map;
}

namespace Files
{
    struct ConfigurationManager;
}

namespace ESM
{
    struct Position;
    struct Cell;
    struct NPC;
    struct Region;
}

namespace RtxTool
{
    /// A Morrowind installation as it sits on disk: the content files, the virtual file system they
    /// are read through, and the indices over them.
    ///
    /// **Separate from `World` because this is the expensive half and the immutable one, and the
    /// two facts are the same fact.** Reading and merging the content files costs some eighty
    /// milliseconds and answers the same thing every time; standing a world on the result costs a
    /// tenth of one and is mutated by whoever holds it — the terrain it builds, the paging it was
    /// asked for, the resources it cached. A harness that wants many worlds over one installation
    /// therefore pays the eighty once, and every world it hands out is its own.
    ///
    /// Nothing here is a function of any world, so a second world changes no answer this gives.
    ///
    /// **Process-wide state is set here**, because it is set from the content and there is one
    /// content: the fallback map, what a hidden node is masked with, and whether a loaded model
    /// records what its surfaces are.
    class Content
    {
    public:
        /// @param config where `openmw.cfg` says the game is installed. Read during construction
        ///        and not held.
        /// @param variables the parsed command line and configuration. Read during construction and
        ///        not held.
        /// @param resourcePath the tree the build wrote `vfs` and `shaders` into.
        Content(Files::ConfigurationManager& config, const boost::program_options::variables_map& variables,
            const std::filesystem::path& resourcePath);
        ~Content();

        Content(const Content&) = delete;
        Content& operator=(const Content&) = delete;

        /// Finds a cell the way Morrowind addresses one: a pair of integers is an exterior, anything
        /// else is an interior's name. Null when there is no such cell.
        const ESM::Cell* findCell(std::string_view spec) const;

        /// The region a cell names, or null where it names none or names one nothing defines.
        ///
        /// **What decides which weathers a place ever sees.** Interiors mostly name nothing, and the
        /// caller reads that as "no opinion" rather than "no weather".
        const ESM::Region* findRegion(const ESM::RefId& id) const;

        /// A game setting's number, or `missing` where the content files carry no such setting.
        ///
        /// **For the handful of constants that are settings rather than fallbacks.** Most of what
        /// the weather is made of comes out of `Fallback::Map`, which reads the ini and needs no
        /// store; `fStromWindSpeed` is the exception and the game reads it from here too.
        float findGameSetting(std::string_view id, float missing) const;

        /// One object a cell places.
        struct Object
        {
            /// Empty for a person, and for a light with no mesh.
            VFS::Path::Normalized mModel;
            osg::Matrixf mTransform;

            /// What this reference lights the cell with, or nothing. A candle is both things at
            /// once: a mesh to place and a light to cast, arriving by the same reference. A pulse
            /// light is only the second, and arrives with `mModel` empty.
            ///
            /// **The description and not the record**, because that is what every rule about a light
            /// reads and what `Terrain::ObjectStorage::getLight` hands over for the reach around a
            /// cell. Reduced once here rather than at each place that asks.
            std::optional<SceneUtil::LightCommon> mLight;

            /// The `NPC_` record this reference stands for, or null.
            ///
            /// **A person arrives with no model at all.** Everyone else names a file; an NPC record
            /// names a race and a sex, and the body has to be assembled out of the `BODY` records
            /// those call for. So the reference hands over the record and `mModel` stays empty.
            const ESM::NPC* mPerson = nullptr;
        };

        /// What `forEachObject` met but could not place.
        struct SkippedObjects
        {
            /// References whose record type is none of the model-bearing ones
            /// `EsmLoader::ModelRecords` lists, so there is nothing to look a model up in.
            std::uint32_t mUnknownType = 0;

            /// References whose record has no model and casts no light. Markers, mostly: a `LIGH`
            /// with no mesh is handed over rather than counted here.
            std::uint32_t mNoModel = 0;
        };

        /// Calls `handle` for every object the cell places: a model to draw, a light to cast, or a
        /// person to assemble.
        SkippedObjects forEachObject(const ESM::Cell& cell, const std::function<void(const Object&)>& handle) const;

        /// Where the game would stand a character who walked into `destination`, if anything leads
        /// there.
        ///
        /// **The arrival a door names, and not the door itself.** A teleporting reference carries the
        /// position its far side puts you at, so what says where an interior is entered is the door
        /// *outside* it — the one in the cell you came from, whose destination is this one. A camera
        /// placed from the interior's own door stands at the way out and looks back in, which is a
        /// different place and, in a winding cave, a wall.
        ///
        /// **Found by walking the world's references, and only when asked.** Nothing indexes this at
        /// load: it would cost every run a pass over every reference in the game to answer a question
        /// only a view with no camera ever asks. The walk stops at the first door that names
        /// `destination`, and what it found is kept for the rest of the run.
        std::optional<ESM::Position> findArrival(const ESM::Cell& destination) const;

        /// Every record of one type the content files carry, sorted by id.
        ///
        /// For what a cell reference cannot answer: a person is assembled out of the `BODY` records
        /// their race calls for, and nothing places those — they are looked up, not referenced.
        template <class T>
        const std::vector<T>& getRecords() const
        {
            return mEsmData.get<T>();
        }

        /// One record by id, or null. Linear over the type, which is what the callers want it for:
        /// a handful of lookups while something is being built, never a frame.
        template <class T>
        const T* findRecord(const ESM::RefId& id) const
        {
            for (const T& record : getRecords<T>())
                if (record.mId == id)
                    return &record;

            return nullptr;
        }

        const EsmLoader::EsmData& getEsmData() const { return mEsmData; }

        const VFS::Manager& getVfs() const { return mVfs; }

        const ToUTF8::StatelessUtf8Encoder& getStatelessEncoder() const { return mEncoder.getStatelessEncoder(); }

        /// What the paging reads the world out of. See `ObjectStorage`.
        const Terrain::ObjectStorage& getObjectStorage() const { return mObjectStorage; }

        /// The tree the build wrote `vfs` and `shaders` into.
        const std::filesystem::path& getResourcePath() const { return mResourcePath; }

    private:
        std::filesystem::path mResourcePath;

        // Every BSA the virtual file system opens keeps the encoder's address, so the encoder is
        // declared first and destroyed last.
        ToUTF8::Utf8Encoder mEncoder;
        Files::Collections mFileCollections;
        VFS::Manager mVfs;

        // Mutable because reading a cell's references borrows a reader and gives it back, which is
        // a cache and not a change to what the content says.
        mutable ESM::ReadersCache mReaders;

        // Built in the initialiser list: `EsmData` is move-constructible and not assignable, which is
        // the right shape for something this size and means it cannot be filled in from the body.
        EsmLoader::EsmData mEsmData;

        // Before `mObjectStorage`, which reads it and does not own it.
        ExteriorIndex mExteriors;

        ObjectStorage mObjectStorage;

        /// Arrivals found so far, by the cell they lead to. A walk that found nothing is remembered
        /// as nothing, so a second ask for the same cell does not walk the world again.
        mutable std::map<std::string, std::optional<ESM::Position>, Misc::StringUtils::CiComp> mArrivals;
    };
}

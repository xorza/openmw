#include "content.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <boost/program_options/variables_map.hpp>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/variant.hpp>
#include <components/esmloader/lessbyid.hpp>
#include <components/esmloader/load.hpp>
#include <components/esmloader/record.hpp>
#include <components/fallback/fallback.hpp>
#include <components/fallback/validate.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/sceneutil/vismask.hpp>
#include <components/settings/values.hpp>
#include <components/surface/material.hpp>
#include <components/vfs/registerarchives.hpp>

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        using StringsVector = std::vector<std::string>;

        ToUTF8::Utf8Encoder makeEncoder(const bpo::variables_map& variables)
        {
            const std::string encoding(variables["encoding"].as<std::string>());
            Log(Debug::Info) << ToUTF8::encodingUsingMessage(encoding);
            return ToUTF8::Utf8Encoder(ToUTF8::calculateEncoding(encoding));
        }

        Files::Collections makeFileCollections(
            Files::ConfigurationManager& config, const bpo::variables_map& variables, const std::filesystem::path& res)
        {
            Files::PathContainer dataDirs(
                Files::asPathContainer(variables["data"].as<Files::MaybeQuotedPathContainer>()));

            auto local = variables["data-local"].as<Files::MaybeQuotedPathContainer::value_type>();
            if (!local.empty())
                dataDirs.push_back(std::move(local));

            config.filterOutNonExistingPaths(dataDirs);
            dataDirs.insert(dataDirs.begin(), res / "vfs");
            return Files::Collections(dataDirs);
        }

        EsmLoader::EsmData loadContent(const bpo::variables_map& variables, const Files::Collections& fileCollections,
            ESM::ReadersCache& readers, ToUTF8::Utf8Encoder& encoder)
        {
            StringsVector contentFiles{ "builtin.omwscripts" };
            const auto& configured = variables["content"].as<StringsVector>();
            contentFiles.insert(contentFiles.end(), configured.begin(), configured.end());

            EsmLoader::Query query;
            query.mLoadCells = true;
            query.mLoadGameSettings = true;
            query.mLoadLands = true;
            query.mLoadLandTextures = true;

            // **A cell names its region and the region names which weathers ever happen there**,
            // which is what the window's weather keys walk. Nothing else reads them.
            query.mLoadRegions = true;
            // Everything that names a model, because everything a cell places has to be rendered:
            // a room missing its lamps and its bookshelves is not the room.
            query.mLoadAllModels = true;
            return EsmLoader::loadEsmData(query, contentFiles, fileCollections, readers, &encoder);
        }

        /// A cell reference reduced to what placing a model needs.
        struct PlacedRef
        {
            ESM::RefNum mRefNum;
            ESM::RefId mRefId;
            ESM::RecNameInts mType;
            ESM::Position mPos;
            float mScale;
        };

        osg::Matrixf makeTransform(const PlacedRef& ref)
        {
            // Scale, then rotate, then translate. The rotation is Misc::Convert's, which applies the
            // reference's Euler angles Z first — the order the original engine used, and not the one
            // a reader of the record would assume.
            osg::Matrixf transform;
            transform.makeRotate(Misc::Convert::makeOsgQuat(ref.mPos));
            transform.preMultScale(osg::Vec3f(ref.mScale, ref.mScale, ref.mScale));
            transform.setTrans(ref.mPos.asVec3());
            return transform;
        }
    }

    Content::Content(Files::ConfigurationManager& config, const bpo::variables_map& variables,
        const std::filesystem::path& resourcePath)
        : mResourcePath(resourcePath)
        , mEncoder(makeEncoder(variables))
        , mFileCollections(makeFileCollections(config, variables, resourcePath))
        , mEsmData(loadContent(variables, mFileCollections, mReaders, mEncoder))
        , mExteriors(mEsmData)
        , mObjectStorage(mEsmData, mExteriors)
    {
        Fallback::Map::init(variables["fallback"].as<Fallback::FallbackMap>().mMap);

        // **Before a single model is read, because this is what a hidden node will carry.** It may
        // not be nothing: a node with no bits at all is skipped by the update traversal too, so the
        // `NifOsg::VisController` that would show it later never runs and what the content hid at
        // load stays hidden for the run. The game names the same bit, and `Rtx::SceneExtractor` and
        // `Terrain::ObjectPaging` both ask the loader for it rather than being told twice.
        //
        // Collision-disabled nodes keep their ordinary mask, which is right where nothing tests for
        // an intersection: the mirror is owed those nodes, since a switch that stops a crosshair
        // does not stop a ray.
        NifOsg::Loader::configure({
            .mHiddenNodeMask = SceneUtil::Mask_UpdateVisitor,
            .mSoftEffects = Settings::shaders().mSoftParticles,
        });

        // Before anything is read, because it decides what reading a model records. Nothing but a
        // renderer that traces asks what the content says a surface is.
        Surface::describeSurfaces(true);

        const auto& archives = variables["fallback-archive"].as<StringsVector>();
        VFS::registerArchives(&mVfs, mFileCollections, archives, true, &mEncoder.getStatelessEncoder());
    }

    Content::~Content() = default;

    const ESM::Region* Content::findRegion(const ESM::RefId& id) const
    {
        if (id.empty())
            return nullptr;

        for (const ESM::Region& region : mEsmData.mRegions)
            if (region.mId == id)
                return &region;

        return nullptr;
    }

    float Content::findGameSetting(std::string_view id, float missing) const
    {
        const ESM::Variant value = EsmLoader::getGameSetting(mEsmData.mGameSettings, id);
        return value.getType() == ESM::VT_Float || value.getType() == ESM::VT_Int ? value.getFloat() : missing;
    }

    const ESM::Cell* Content::findCell(std::string_view spec) const
    {
        // Morrowind's own addressing: a pair of integers is an exterior, anything else is a name.
        const std::size_t comma = spec.find(',');
        if (comma != std::string_view::npos)
        {
            int x = 0;
            int y = 0;
            const std::string_view first = spec.substr(0, comma);
            const std::string_view second = spec.substr(comma + 1);
            const auto parsedX = std::from_chars(first.data(), first.data() + first.size(), x);
            const auto parsedY = std::from_chars(second.data(), second.data() + second.size(), y);

            if (parsedX.ec == std::errc() && parsedX.ptr == first.data() + first.size() && parsedY.ec == std::errc()
                && parsedY.ptr == second.data() + second.size())
                return mExteriors.find(x, y);
        }

        for (const ESM::Cell& cell : mEsmData.mCells)
            if (!cell.isExterior() && Misc::StringUtils::ciEqual(cell.mName, spec))
                return &cell;

        return nullptr;
    }

    std::optional<ESM::Position> Content::findArrival(const ESM::Cell& destination) const
    {
        // An exterior has no name to be named by, and nothing teleports to one by name.
        if (destination.isExterior())
            return std::nullopt;

        if (const auto known = mArrivals.find(destination.mName); known != mArrivals.end())
            return known->second;

        // **From outside if anything leads in from outside.** A room is entered from the street far
        // more often than from the room behind it, and a back door lands you facing the wrong way
        // through a building. An interior source is taken only where no exterior one exists at all.
        std::optional<ESM::Position> arrival;
        std::optional<ESM::Position> fromInside;

        for (const ESM::Cell& cell : mEsmData.mCells)
        {
            if (arrival.has_value())
                break;

            for (std::size_t i = 0; i < cell.mContextList.size() && !arrival.has_value(); ++i)
            {
                const ESM::ReadersCache::BusyItem reader
                    = mReaders.get(static_cast<std::size_t>(cell.mContextList[i].index));
                cell.restore(*reader, static_cast<int>(i));

                ESM::CellRef ref;
                bool deleted = false;
                while (ESM::Cell::getNextRef(*reader, ref, deleted))
                {
                    // **The flag and not the name.** A reference carries a destination cell whether
                    // or not it is a way through — an ordinary door in a house names the room it
                    // belongs to — and only a teleporting one puts anybody anywhere.
                    if (deleted || !ref.mTeleport || !Misc::StringUtils::ciEqual(ref.mDestCell, destination.mName))
                        continue;

                    if (cell.isExterior())
                    {
                        arrival = ref.mDoorDest;
                        break;
                    }

                    if (!fromInside.has_value())
                        fromInside = ref.mDoorDest;
                }
            }
        }

        if (!arrival.has_value())
            arrival = fromInside;

        mArrivals.emplace(destination.mName, arrival);
        return arrival;
    }

    Content::SkippedObjects Content::forEachObject(
        const ESM::Cell& cell, const std::function<void(const Object&)>& handle) const
    {
        // A later content file can move or delete a reference an earlier one placed, so the refs are
        // gathered and reduced by reference number before any of them is drawn. Skipping this draws
        // the mod's version of an object on top of the original's.
        EsmLoader::Records<PlacedRef> gathered;

        // **What a later file moved out of this cell, which no block below can say.** Only the file
        // that moved a reference carries the `MVRF`, so reading an earlier file's block finds it
        // exactly where it used to stand — and drawing it there is the same object twice, once in
        // each cell.
        const auto departed = [&](const ESM::RefNum& refNum) {
            return std::find(cell.mMovedRefs.begin(), cell.mMovedRefs.end(), refNum) != cell.mMovedRefs.end();
        };

        const auto typeOf = [&](const ESM::RefId& id) {
            const auto found
                = std::lower_bound(mEsmData.mRefIdTypes.begin(), mEsmData.mRefIdTypes.end(), id, EsmLoader::LessById{});
            return found == mEsmData.mRefIdTypes.end() || found->mId != id ? ESM::RecNameInts{} : found->mType;
        };

        for (std::size_t i = 0; i < cell.mContextList.size(); ++i)
        {
            const ESM::ReadersCache::BusyItem reader
                = mReaders.get(static_cast<std::size_t>(cell.mContextList[i].index));
            cell.restore(*reader, static_cast<int>(i));

            ESM::CellRef ref;
            bool deleted = false;
            while (ESM::Cell::getNextRef(*reader, ref, deleted))
            {
                if (departed(ref.mRefNum))
                    continue;

                gathered.emplace_back(
                    deleted, PlacedRef{ ref.mRefNum, std::move(ref.mRefID), typeOf(ref.mRefID), ref.mPos, ref.mScale });
            }
        }

        // **And what a later file moved in, which this cell's own blocks never mention.** The
        // reference belongs to the cell it came from as far as every block is concerned; the only
        // record that it stands here is the lease the loader attached.
        for (const auto& [leased, deleted] : cell.mLeasedRefs)
        {
            gathered.emplace_back(
                deleted, PlacedRef{ leased.mRefNum, leased.mRefID, typeOf(leased.mRefID), leased.mPos, leased.mScale });
        }

        const std::vector<PlacedRef> refs = EsmLoader::prepareRecords(
            gathered, [](const EsmLoader::Record<PlacedRef>& v) -> ESM::RefNum { return v.mValue.mRefNum; });

        SkippedObjects skipped;
        for (const PlacedRef& ref : refs)
        {
            if (ref.mType == ESM::RecNameInts{})
            {
                ++skipped.mUnknownType;
                continue;
            }

            // Before the model check, because a person has none: their body is assembled from the
            // records their race calls for and the reference names only who they are.
            if (ref.mType == ESM::REC_NPC_)
            {
                handle(Object{
                    .mTransform = makeTransform(ref),
                    .mPerson = EsmLoader::find<ESM::NPC>(mEsmData, ref.mRefId),
                });
                continue;
            }

            const ESM::Light* record
                = ref.mType == ESM::REC_LIGH ? EsmLoader::find<ESM::Light>(mEsmData, ref.mRefId) : nullptr;

            std::optional<SceneUtil::LightCommon> light;
            if (record != nullptr)
                light.emplace(*record);

            VFS::Path::Normalized model(EsmLoader::getModel(mEsmData, ref.mRefId, ref.mType));
            if (model.empty())
            {
                // **A light with no mesh still burns, and the game places it** —
                // `MWClass::Light::insertObjectRendering` inserts the reference "even if model is
                // empty, so that the light is added". A propylon chamber is lit by nothing else:
                // eight `blue_128_pulse` and `purp_01_128_pulse` records in a room whose ambient is
                // fifteen over 255, and skipping them with the markers rendered the room black.
                if (!light.has_value())
                {
                    ++skipped.mNoModel;
                    continue;
                }

                handle(Object{ .mTransform = makeTransform(ref), .mLight = light });
                continue;
            }

            if (ref.mType != ESM::REC_STAT)
                model = Misc::ResourceHelpers::correctActorModelPath(model, &mVfs);

            handle(Object{
                .mModel = Misc::ResourceHelpers::correctMeshPath(model),
                .mTransform = makeTransform(ref),
                .mLight = light,
            });
        }

        return skipped;
    }
}

#include "world.hpp"

#include <components/esm3/variant.hpp>

#include <algorithm>
#include <charconv>
#include <vector>

#include <boost/program_options/variables_map.hpp>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esmloader/lessbyid.hpp>
#include <components/esmloader/load.hpp>
#include <components/esmloader/record.hpp>
#include <components/fallback/fallback.hpp>
#include <components/fallback/validate.hpp>
#include <components/files/collections.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/terrain/chunkmanager.hpp>
#include <components/terrain/objectpaging.hpp>
#include <components/terrain/quadtreeworld.hpp>
#include <components/terrain/terraingrid.hpp>
#include <components/vfs/registerarchives.hpp>

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        using StringsVector = std::vector<std::string>;

        /// Nothing here is drawn, so caching a resource past its last use only costs memory.
        constexpr double sExpiryDelay = 0;

        /// What the shader visitor is told a GPU offers.
        ///
        /// It runs on every model OpenMW loads and needs a number to fit texture slots into. Without
        /// a context there is nothing to ask, and the value only decides how many slots it is willing
        /// to use — the roles it labels them with, which is what this tool reads, are the same.
        constexpr int sAssumedTextureUnits = 32;

        /// The defines every shader template expects to have been told before it can be assembled.
        ///
        /// The game fills these in from a realised GL context. There is none here, and the values do
        /// not matter: nothing this tool builds will be compiled by a driver. What matters is that
        /// every name the templates reference is defined, because an undefined one throws and takes
        /// the model that triggered it with it.
        Shader::ShaderManager::DefineMap makeGlobalDefines(Resource::ResourceSystem& resourceSystem)
        {
            Shader::ShaderManager::DefineMap defines = Shader::getDefaultDefines();

            for (const auto& [name, value] : SceneUtil::ShadowManager::getShadowsDisabledDefines())
                defines[name] = value;

            const osg::ref_ptr<SceneUtil::LightManager> lights
                = new SceneUtil::LightManager(SceneUtil::LightSettings{}, &resourceSystem);
            for (const auto& [name, value] : lights->getLightDefines())
                defines[name] = value;

            return defines;
        }

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
            query.mModels = EsmLoader::allModelRecords();
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

    World::World(Files::ConfigurationManager& config, const bpo::variables_map& variables,
        const std::filesystem::path& resourcePath)
        : mEncoder(makeEncoder(variables))
        , mFileCollections(makeFileCollections(config, variables, resourcePath))
        , mEsmData(loadContent(variables, mFileCollections, mReaders, mEncoder))
        , mObjectStorage(mEsmData)
    {
        Fallback::Map::init(variables["fallback"].as<Fallback::FallbackMap>().mMap);

        const auto& archives = variables["fallback-archive"].as<StringsVector>();
        VFS::registerArchives(&mVfs, mFileCollections, archives, true, &mEncoder.getStatelessEncoder());

        mResourceSystem
            = std::make_unique<Resource::ResourceSystem>(&mVfs, sExpiryDelay, &mEncoder.getStatelessEncoder());

        Resource::SceneManager& sceneManager = *mResourceSystem->getSceneManager();

        // The shader visitor runs on every model as it loads and is what labels a texture slot
        // "diffuseMap" or "normalMap". Those labels are the only record of what a texture is for, so
        // the tool needs it to run — and it throws when a program will not build, which takes the
        // whole model down with it. Nothing here will ever be compiled by a driver.
        sceneManager.setShaderPath(resourcePath / "shaders");
        sceneManager.getShaderManager().setMaxTextureUnits(sAssumedTextureUnits);
        // Taken by non-const reference: the shader manager reserves the right to add to it.
        Shader::ShaderManager::DefineMap globalDefines = makeGlobalDefines(*mResourceSystem);
        sceneManager.getShaderManager().setGlobalDefines(globalDefines);
        sceneManager.setAutoUseNormalMaps(Settings::shaders().mAutoUseObjectNormalMaps);
        sceneManager.setNormalMapPattern(Settings::shaders().mNormalMapPattern);
        sceneManager.setNormalHeightMapPattern(Settings::shaders().mNormalHeightMapPattern);
        sceneManager.setAutoUseSpecularMaps(Settings::shaders().mAutoUseObjectSpecularMaps);
        sceneManager.setSpecularMapPattern(Settings::shaders().mSpecularMapPattern);
    }

    osg::ref_ptr<osg::Group> World::buildTerrain(const ESM::Cell& cell)
    {
        if (!cell.isExterior())
        {
            // **Turned off rather than left standing.** A run that staged an exterior first still
            // holds the world that has ground in it, and a paged world reached through `collect`
            // does not stop answering merely because nothing added it to this cell's graph. This is
            // what the game does when the player goes inside.
            if (mTerrain != nullptr)
                mTerrain->enable(false);

            return nullptr;
        }

        if (mTerrain == nullptr)
        {
            mTerrainStorage = std::make_unique<TerrainStorage>(mVfs, mEsmData);
            mTerrainParent = new osg::Group;

            // `Terrain::World` hangs a pre-render camera off this to build composite maps. Nothing
            // asks it for one — `Terrain::sNoCompositeMap` below is what says so — and nothing here
            // ever draws either, so the camera is inert and every chunk comes out as its layer stack.
            mCompileRoot = new osg::Group;

            if (mPagedTerrain)
            {
                // **The same world the game builds with `distant terrain` on**, and the reason this
                // is an option at all: `QuadTreeWorld` keeps its chunks out of the scene graph, so
                // it is the one terrain a mirror cannot find by walking. The numbers below are the
                // settings' own defaults, because what is under test is the paging and not a tuning
                // of it — all but the composite map level, which is the one thing this path decides
                // rather than reads.
                auto paged
                    = std::make_unique<Terrain::QuadTreeWorld>(mTerrainParent, mCompileRoot, mResourceSystem.get(),
                        mTerrainStorage.get(), ~0u, ~0u, ~0u, Settings::terrain().mCompositeMapResolution,
                        Terrain::sNoCompositeMap, Settings::terrain().mLodFactor, Settings::terrain().mVertexLodMod,
                        Settings::terrain().mMaxCompositeGeometrySize, false, ESM::Cell::sDefaultWorldspaceId,
                        sExpiryDelay);

                // **The chunk managers the game registers, from the setting the game reads.** A
                // quad tree asks every one of them for its chunk and adds what comes back, so a
                // world that registered none produces ground and nothing else — which is why the
                // harness's distant hillsides arrived bare while the same hillside inside the
                // active grid carried a town. Groundcover is the third the game registers and is
                // not here: it wants its own distance and probably its own answer.
                if (mPagedStatics && Settings::terrain().mObjectPaging)
                {
                    // **The distance only, because this world stands its own active grid.**
                    // `readRegion` places every reference a loaded cell carries, one at a time; a
                    // paging that also merged those cells would stand each of them twice. The game
                    // avoids that by asking `getPagedRefnums` what a chunk swallowed and skipping
                    // it, which needs a `Scene` and chunks already built — neither of which exists
                    // here. Past the active grid, which is what this is for, the two worlds build
                    // the same thing.
                    mObjectPaging = std::make_unique<Terrain::ObjectPaging>(mResourceSystem->getSceneManager(),
                        mObjectStorage, ESM::Cell::sDefaultWorldspaceId, ~0u, /*pageActiveGrid=*/false);
                    paged->addChunkManager(mObjectPaging.get());
                    mResourceSystem->addResourceManager(mObjectPaging.get());
                }

                mResident = std::make_unique<Rtx::TerrainResidency>();
                mResident->follow(paged.get());
                mTerrain = std::move(paged);
            }
            else
                mTerrain = std::make_unique<Terrain::TerrainGrid>(mTerrainParent, mCompileRoot, mResourceSystem.get(),
                    mTerrainStorage.get(), ~0u, ESM::Cell::sDefaultWorldspaceId, sExpiryDelay);

            // `viewing distance` unless something asked for more, which is smaller than a cell and
            // so is the whole of why nothing outside the active grid exists until it is raised.
            mTerrain->setViewDistance(mTerrainViewDistance.value_or(Settings::camera().mViewingDistance));
        }

        // **A grid and not a cell, for the paged world.** It holds what the grid names and nothing
        // else, so a cell arriving widens the square rather than being loaded into it.
        const osg::Vec4i square(cell.getGridX(), cell.getGridY(), cell.getGridX() + 1, cell.getGridY() + 1);
        mActiveGrid = mActiveGrid.has_value()
            ? osg::Vec4i(std::min(mActiveGrid->x(), square.x()), std::min(mActiveGrid->y(), square.y()),
                  std::max(mActiveGrid->z(), square.z()), std::max(mActiveGrid->w(), square.w()))
            : square;
        mTerrain->setActiveGrid(*mActiveGrid);
        mTerrain->enable(true);

        mTerrain->loadCell(cell.getGridX(), cell.getGridY());
        return mTerrainParent;
    }

    void World::setTerrainViewDistance(float units)
    {
        mTerrainViewDistance = units;

        if (mTerrain != nullptr)
            mTerrain->setViewDistance(units);
    }

    void World::setTerrainViewPoint(const osg::Vec3f& where)
    {
        if (mResident != nullptr)
            mResident->setViewPoint(where);
    }

    void World::unloadTerrain(int x, int y)
    {
        // Nothing was ever built, which is every interior and every run that has not seen an
        // exterior yet.
        if (mTerrain != nullptr)
            mTerrain->unloadCell(x, y);
    }

    Resource::SceneManager& World::getSceneManager()
    {
        return *mResourceSystem->getSceneManager();
    }

    Resource::ImageManager& World::getImageManager()
    {
        return *mResourceSystem->getImageManager();
    }

    World::~World() = default;

    const ESM::Region* World::findRegion(const ESM::RefId& id) const
    {
        if (id.empty())
            return nullptr;

        for (const ESM::Region& region : mEsmData.mRegions)
            if (region.mId == id)
                return &region;

        return nullptr;
    }

    float World::findGameSetting(std::string_view id, float missing) const
    {
        const ESM::Variant value = EsmLoader::getGameSetting(mEsmData.mGameSettings, id);
        return value.getType() == ESM::VT_Float || value.getType() == ESM::VT_Int ? value.getFloat() : missing;
    }

    const ESM::Cell* World::findCell(std::string_view spec) const
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
            {
                for (const ESM::Cell& cell : mEsmData.mCells)
                    if (cell.isExterior() && cell.getGridX() == x && cell.getGridY() == y)
                        return &cell;

                return nullptr;
            }
        }

        for (const ESM::Cell& cell : mEsmData.mCells)
            if (!cell.isExterior() && Misc::StringUtils::ciEqual(cell.mName, spec))
                return &cell;

        return nullptr;
    }

    std::optional<ESM::Position> World::findArrival(const ESM::Cell& destination)
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

    World::SkippedObjects World::forEachObject(const ESM::Cell& cell, const std::function<void(const Object&)>& handle)
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

            VFS::Path::Normalized model(EsmLoader::getModel(mEsmData, ref.mRefId, ref.mType));
            if (model.empty())
            {
                ++skipped.mNoModel;
                continue;
            }

            if (ref.mType != ESM::REC_STAT)
                model = Misc::ResourceHelpers::correctActorModelPath(model, &mVfs);

            handle(Object{
                .mModel = Misc::ResourceHelpers::correctMeshPath(model),
                .mTransform = makeTransform(ref),
                .mLight = ref.mType == ESM::REC_LIGH ? EsmLoader::find<ESM::Light>(mEsmData, ref.mRefId) : nullptr,
            });
        }

        return skipped;
    }
}

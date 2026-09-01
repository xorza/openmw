#include "world.hpp"

#include <components/esm3/loadcell.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/distantland.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/surface/material.hpp>
#include <components/terrain/chunkmanager.hpp>
#include <components/terrain/objectpaging.hpp>
#include <components/terrain/quadtreeworld.hpp>
#include <components/terrain/terraingrid.hpp>

#include "content.hpp"

namespace RtxTool
{
    namespace
    {
        /// How long a resource outlives its last use, which is the game's own number.
        ///
        /// **Not nought, and the crossing line is why.** A route bench walks back into cells it has
        /// just left, and a cache that expired on the frame a mesh stopped being named re-read from
        /// the archive what the game would still have been holding — so the `reading` half of a
        /// crossing was inflated by exactly what the cache was there to keep.
        double expiryDelay()
        {
            return Settings::cells().mCacheExpiryDelay;
        }

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
    }

    float landReach()
    {
        return Rtx::distantLandReach(Settings::rtx().mDistantLandCells, Settings::camera().mViewingDistance);
    }

    bool gameMergesActiveGridStatics()
    {
        return Settings::terrain().mObjectPaging && Settings::terrain().mObjectPagingActiveGrid;
    }

    World::World(const Content& content)
        : mContent(content)
        , mResourceSystem(std::make_unique<Resource::ResourceSystem>(
              &content.getVfs(), expiryDelay(), &content.getStatelessEncoder()))
    {
        Resource::SceneManager& sceneManager = *mResourceSystem->getSceneManager();

        // The shader visitor runs on every model as it loads and is what labels a texture slot
        // "diffuseMap" or "normalMap". Those labels are the only record of what a texture is for, so
        // the tool needs it to run — and it throws when a program will not build, which takes the
        // whole model down with it. Nothing here will ever be compiled by a driver.
        sceneManager.setShaderPath(content.getResourcePath() / "shaders");
        sceneManager.getShaderManager().setMaxTextureUnits(Surface::sAssumedTextureUnits);
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
            mTerrainStorage = std::make_unique<TerrainStorage>(mContent.getVfs(), mContent.getEsmData());
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
                        expiryDelay());

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
                        mContent.getObjectStorage(), ESM::Cell::sDefaultWorldspaceId, ~0u, /*pageActiveGrid=*/false);
                    paged->addChunkManager(mObjectPaging.get());
                    mResourceSystem->addResourceManager(mObjectPaging.get());
                }

                mResident = std::make_unique<Rtx::TerrainResidency>();
                mResident->follow(paged.get());
                mTerrain = std::move(paged);

                mDistantLights.follow(&mContent.getObjectStorage(), ESM::Cell::sDefaultWorldspaceId);
                mResidencies = { mResident.get(), &mDistantLights };
            }
            else
                mTerrain = std::make_unique<Terrain::TerrainGrid>(mTerrainParent, mCompileRoot, mResourceSystem.get(),
                    mTerrainStorage.get(), ~0u, ESM::Cell::sDefaultWorldspaceId, expiryDelay());

            // `viewing distance` unless something asked for more, which is smaller than a cell and
            // so is the whole of why nothing outside the active grid exists until it is raised.
            mTerrain->setViewDistance(mTerrainViewDistance.value_or(Settings::camera().mViewingDistance));
        }

        // The terrain may have been stood up by this very call, so the grid the caller already
        // named is installed here rather than only in `setActiveCellGrid`.
        mTerrain->setActiveGrid(mActiveGrid.getBounds());
        mTerrain->enable(true);

        mTerrain->loadCell(cell.getGridX(), cell.getGridY());
        return mTerrainParent;
    }

    void World::beginStaging()
    {
        SceneUtil::resetLightIds();
        mDistantLights.restart();
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

        mDistantLights.setViewPoint(where);

        // **Told here rather than at construction**, because `--distant-cells` moves the setting
        // after this world exists. `RtxRenderer` says it beside its own view point for the same
        // reason.
        mDistantLights.setReach(landReach());
    }

    void World::setActiveCellGrid(const Misc::CellGrid& grid)
    {
        mActiveGrid = grid;

        // Null until the first exterior cell arrives, and `buildTerrain` installs what it finds
        // here when it does.
        if (mTerrain != nullptr)
            mTerrain->setActiveGrid(grid.getBounds());

        // **The same grid, and for the same reason `readRegion` gives.** This world places every
        // reference a loaded cell carries, lights included, so the cells it has loaded are exactly
        // the ones the distant lights must leave alone.
        mDistantLights.setActiveGrid(grid.getBounds());
    }

    void World::unloadTerrain(int x, int y)
    {
        // Nothing was ever built, which is every interior and every run that has not seen an
        // exterior yet.
        if (mTerrain != nullptr)
            mTerrain->unloadCell(x, y);
    }

    void World::clearTerrain()
    {
        // Backwards through what `buildTerrain` stood up, which is the order the members are
        // declared in and for the reasons given there. The residency is ahead of all of it: the
        // view it holds was handed out by the world about to go.
        mResidencies.clear();
        mResident.reset();
        mTerrain.reset();

        if (mObjectPaging != nullptr)
        {
            // The paging is the one manager registered by hand rather than by its own constructor,
            // so it is the one that has to be taken out by hand.
            mResourceSystem->removeResourceManager(mObjectPaging.get());
            mObjectPaging.reset();
        }

        mCompileRoot = nullptr;
        mTerrainParent = nullptr;
        mTerrainStorage.reset();
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
}

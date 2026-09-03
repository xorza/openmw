#pragma once

#include <cstddef>

#include <components/rtx/distantlights.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/sceneuploader.hpp>
#include <components/rtx/skybuilder.hpp>
#include <components/rtx/terrainresidency.hpp>

namespace Resource
{
    class ImageManager;
    class ResourceSystem;
}

namespace Rtx
{
    class Renderer;
}

namespace MWRender
{
    struct SceneFrame;

    /// How much world this renderer builds, in units.
    ///
    /// **One reading for the game, because `components/rtx` holds no settings registry.** The
    /// ground, the air and the distant lights are all measured over the same number, and a host that
    /// answered the question twice could build ground to one reach and air to another.
    float landReach();

    /// The engine's scene graph mirrored into what a ray can meet.
    ///
    /// **Everything between "the game has a frame" and "trace it".** The walk, what it walks past,
    /// what stands for the ground and the lights the game has already placed, and the hand-over that
    /// decides whether the device is placed, extended or rebuilt. Nothing here touches a window, an
    /// event, the interface or a benchmark.
    ///
    /// **The maps live across frames**, which is what makes the mirror incremental: the same crate
    /// met again resolves to the mesh already uploaded rather than to a copy of it, and a cell that
    /// left gives its slots back on the frame the sweep misses it.
    class WorldMirror
    {
    public:
        WorldMirror();

        /// The resource system the sky's own meshes are loaded through. Told once, where the world
        /// is attached.
        void attach(Resource::ResourceSystem& resources) { mResources = &resources; }

        /// Walks this frame's world into the scene, and says what the walk found.
        ///
        /// The precipitation goes in as a second root: those nodes hang under the sky's
        /// camera-relative transform, which the first walk is masked out of.
        Rtx::ExtractionStats mirror(const SceneFrame& frame, std::size_t frameNumber);

        /// Hands the scene to `renderer`, building only what has to be built.
        Rtx::SceneUpload hand(Rtx::Renderer& renderer, Resource::ImageManager& images);

        /// Catches the walk up and drops what it did not find.
        ///
        /// **After the trace and not before the walk.** Where everything stood this frame is what
        /// the next one measures its motion against, and the sweep bumps the epoch that measurement
        /// is made against.
        void settle();

        const Rtx::SceneDesc& getScene() const { return mScene; }
        Rtx::SceneDesc& getScene() { return mScene; }

        /// Where every walk that can reach one graph takes its traversal numbers from.
        Rtx::Traversals& getTraversals() { return mTraversals; }

        /// What the scene holds of the dome, the clouds, the stars and the moons.
        const Rtx::SkyContent& getSky() const { return mSkyContent; }
        const Rtx::MoonFaces& getMoonFaces() const { return mMoonFaces; }

    private:
        /// Shared by everything that can reach one graph — the world's walk and every traced view.
        Rtx::Traversals mTraversals;

        Rtx::SceneDesc mScene;
        Rtx::SceneExtractor mExtractor;

        /// The moons' portraits and the sky's own meshes, added once and never given back.
        Rtx::MoonFaces mMoonFaces;
        Rtx::SkyContent mSkyContent;

        /// What stands for the terrain a paged world has not built, and for the lights of the cells
        /// it stands for.
        Rtx::TerrainResidency mResident;
        Rtx::DistantLights mDistantLights;

        Rtx::SceneUploader mUploader;

        /// Where the sky's meshes are loaded from. Borrowed: the world outlives this.
        Resource::ResourceSystem* mResources = nullptr;

        /// Where the world's clock stood on the last frame, so the emitters are given the gap.
        double mLastSimulationTime = 0.0;
    };
}

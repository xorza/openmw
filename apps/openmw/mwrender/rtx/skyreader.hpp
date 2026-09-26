#pragma once

#include <vector>

#include <osg/Vec3f>

#include <components/rtx/frameworld.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/skybuilder.hpp>
#include <components/sky/skyclock.hpp>
#include <components/vfs/pathutil.hpp>

namespace Resource
{
    class SceneManager;
}

namespace VFS
{
    class Manager;
}

namespace Rtx
{
    class SceneDesc;
}

namespace MWRender
{
    class Precipitation;
    struct SkyState;
    struct WorldState;

    /// Turns what the game says about a frame's world into what the renderer builds a sky, an air
    /// and a sea out of: the colours decoded, whether the cell has a sky decided, and every reading
    /// handed to the one builder that decides what a sun, a room light, an air and a moon may be —
    /// which is what keeps the game and the harness under the same sky. Nothing here touches a
    /// device, and nothing here is a decision this host makes on its own.
    ///
    /// Apart from `WorldMirror`, which is the walk and the hand-over: what this holds is read off
    /// the content once and never off the graph, and `read` is a function of the frame's records
    /// and the one clock this keeps.
    class SkyReader
    {
    public:
        /// Reads the fallback map's few sky constants once: what a script paints Secunda and the
        /// sun glare fader's three numbers.
        SkyReader();

        /// Every file `attach` reads that the game may load ahead of the first cell: the cloud
        /// shell, the star sphere and the two moons' faces. Listed here, beside what reads them,
        /// so the two cannot name different files. The second star sphere is an expansion's and is
        /// listed only where `vfs` holds it, because a missing model aborts the whole preload.
        static void listAssets(const VFS::Manager& vfs, std::vector<VFS::Path::Normalized>& models,
            std::vector<VFS::Path::Normalized>& textures);

        /// Adds the moons' portraits and the sky's sheets to `scene` and holds them there for the
        /// life of the scene: a moon and a deck are drawn by a ray that reached nothing, so no
        /// material speaks for the slots and the sweep would take them on the first frame a cell
        /// died. Once, where the world is attached.
        void attach(Rtx::SceneDesc& scene, Resource::SceneManager& scenes);

        /// Gives every hold `attach` took back to `scene`, so a scene the world has left holds
        /// nothing of the sky: `attach`'s pair, where the world is detached.
        void detach(Rtx::SceneDesc& scene);

        /// Moves the sky's clocks on by one frame: the deck's scroll and the seconds the fog drifts
        /// by. Every unpaused frame the sky is on, as the rasterizer's dome
        /// steps its own.
        void step(float seconds, float timeScale, float cloudSpeed) { mClock.step(seconds, timeScale, cloudSpeed); }

        /// @param falling what the weather drops, for how much of it rings the water and how high
        ///        a roof shelters from it.
        /// @param seconds the world's clock, which the sea is animated by.
        /// @param reach how far the world is built, in units, which the open air closes over.
        Rtx::WorldReading read(const SkyState& sky, const WorldState& world, const Precipitation& falling,
            double seconds, float reach) const;

    private:
        /// The sky's own meshes, as the settings name them.
        static Rtx::SkyMeshes meshes();

        /// The moons' portraits and the sky's own meshes, held from `attach` to `detach`.
        Rtx::MoonFaces mMoonFaces;
        Rtx::SkyContent mSkyContent;

        /// The deck and the fog's seconds, this renderer's own: the dome keeps its own deck and
        /// neither reads the other's.
        Sky::SkyClock mClock;

        /// What a script paints Secunda, `Moons_Script_Color` decoded, read once as the
        /// rasterizer's `SkyManager` reads it. `WorldState::mMoonRed` says when.
        osg::Vec3f mMoonPaint;

        /// The sun glare fader's three constants, read once as `SunGlareCallback` reads them:
        /// `Weather_Sun_Glare_Fader_Color` doubled and clamped, `_Max`, and `_Angle_Max` in
        /// radians. `glare.h` says what each is.
        osg::Vec3f mGlareColour;
        float mGlareMax;
        float mGlareAngleMax;

        /// How big the configuration draws each moon, read with the rest of the fallbacks.
        Rtx::MoonSizes mMoonSizes;
    };
}

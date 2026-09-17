#pragma once

#include <osg/Vec3f>

#include <components/rtx/frameworld.hpp>
#include <components/rtx/moonbuilder.hpp>
#include <components/rtx/skybuilder.hpp>

namespace Resource
{
    class SceneManager;
}

namespace Rtx
{
    class SceneDesc;
}

namespace MWRender
{
    struct WorldState;

    /// Turns what the game says about a frame's world into what the renderer builds a sky, an air
    /// and a sea out of: the colours decoded, whether the cell has a sky decided, and every reading
    /// handed to the one builder that decides what a sun, a room light, an air and a moon may be —
    /// which is what keeps the game and the harness under the same sky. Nothing here touches a
    /// device, and nothing here is a decision this host makes on its own.
    ///
    /// Apart from `WorldMirror`, which is the walk and the hand-over: what this holds is read off
    /// the content once and never off the graph, and `read` is a function of the frame's world.
    class SkyReader
    {
    public:
        /// Reads the fallback map's few sky constants once: what a script paints Secunda and the
        /// sun glare fader's three numbers.
        SkyReader();

        /// The sky's own meshes, as the settings name them: what `attach` reads and what the game
        /// preloads. Read here so the two cannot name different files.
        static Rtx::SkyMeshes meshes();

        /// Adds the moons' portraits and the sky's sheets to `scene` and holds them there for the
        /// life of the scene: a moon and a deck are drawn by a ray that reached nothing, so no
        /// material speaks for the slots and the sweep would take them on the first frame a cell
        /// died. Once, where the world is attached.
        void attach(Rtx::SceneDesc& scene, Resource::SceneManager& scenes);

        /// Gives every hold `attach` took back to `scene`, so a scene the world has left holds
        /// nothing of the sky: `attach`'s pair, where the world is detached.
        void detach(Rtx::SceneDesc& scene);

        /// @param seconds the world's clock, which the sea is animated by.
        /// @param reach how far the world is built, in units, which the open air closes over.
        Rtx::WorldReading read(const WorldState& world, float seconds, float reach) const;

    private:
        /// The moons' portraits and the sky's own meshes, held from `attach` to `detach`.
        Rtx::MoonFaces mMoonFaces;
        Rtx::SkyContent mSkyContent;

        /// What a script paints Secunda, `Moons_Script_Color` decoded, read once as the
        /// rasterizer's `SkyManager` reads it. `SkySettled::mMoonRed` says when.
        osg::Vec3f mMoonPaint;

        /// The sun glare fader's three constants, read once as `SunGlareCallback` reads them:
        /// `Weather_Sun_Glare_Fader_Color` doubled and clamped, `_Max`, and `_Angle_Max` in
        /// radians. `Rtx::Shaders::glare.h` says what each is.
        osg::Vec3f mGlareColour;
        float mGlareMax;
        float mGlareAngleMax;
    };
}

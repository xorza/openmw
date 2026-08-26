#pragma once

#include <cstddef>
#include <memory>
#include <string>

// Complete rather than forward declared, because `getRoot` hands out a `Group` and every caller
// then wants it as the `osg::Node&` an extractor takes.
#include <osg/Group>
#include <osg/Matrixf>

#include <components/vfs/pathutil.hpp>

namespace Rtx
{
    class PoseCull;
}

namespace osg
{
    class FrameStamp;
}

namespace osgUtil
{
    class UpdateVisitor;
}

namespace RtxTool
{
    class World;

    /// What an actor is made of, before anything has posed it.
    ///
    /// **A separate step because the two kinds of actor are assembled differently and posed
    /// identically.** A creature is one skinned file; a person is a shared skeleton with a limb hung
    /// on each bone. Once either exists it is the same problem — bones the keyframes move and skin
    /// that follows them — which is what `Actor` is.
    struct ActorModel
    {
        /// The graph, with a `SceneUtil::Skeleton` at its root.
        osg::ref_ptr<osg::Group> mRoot;

        /// What was actually loaded, which is never quite what was asked for: the `x`-prefixed
        /// skeleton beside a creature's model, or the base animation a person's parts hang on. The
        /// keyframes are the same path with a different extension.
        VFS::Path::Normalized mSkeleton;

        /// Which animation group to stand in, and what to stand in instead where the track has
        /// never heard of it.
        ///
        /// **A person holding a weapon stands differently, and the weapon's bone is only placed by
        /// the idle that goes with it** — hung in the empty-handed idle a sword comes out lying
        /// across its owner. Everything else — a creature, a prop, somebody unarmed — leaves both at
        /// the plain idle. `Actor` tries them in order and then the plain idle, which is the chain
        /// the game walks in `fallbackShortWeaponGroup`.
        std::string mIdle = "idle";
        std::string mIdleFallback = "idle";
    };

    /// A slice of a keyframe track, in the track's own seconds.
    struct Span
    {
        float mStart = 0.0f;
        float mStop = 0.0f;
    };

    /// A creature: one skinned file, with its animation beside it under the same name.
    ///
    /// @param model the reference's model path — `meshes/r/cliffracer.nif`, not the `x`-prefixed
    ///        skinned one. Morrowind keeps an actor's skeleton in a second file beside the static
    ///        model and names it by convention, so the correction happens here and a caller passes
    ///        what a `CREA` record holds.
    ActorModel loadCreature(World& world, VFS::Path::NormalizedView model);

    /// A prop: one ordinary model, as an instance of its own rather than the shared template.
    ///
    /// **An instance, and that is the whole point of the call.** A template is one object handed to
    /// every reference of the model, and its particle systems are one set of particles shared by
    /// thirty candles — frozen, because nothing ever updates a template. `SceneManager::getInstance`
    /// deep-copies the nodes and the particle systems and leaves the plain geometry shared, so each
    /// candle gets a flame of its own and the mesh cache still sees one candle.
    ActorModel loadProp(World& world, VFS::Path::NormalizedView model);

    /// One animated actor, loaded and posed with no game running and no window open.
    ///
    /// **A pose and not an animation system.** `MWRender::Animation` is two thousand lines about
    /// which animation plays, when it stops, what blends into it and how far it moved the thing
    /// playing it — every one of those a question about the simulation. A mirror cannot tell one
    /// pose from another, so this asks the only question it can answer: put the bones where the
    /// keyframes say they are at this time, and skin to them.
    ///
    /// It exists because the harness could not show an actor at all, and a skinned body is the one
    /// thing in the game whose vertices change every frame — so the only way to see whether the RT
    /// path handled one was to open the game and look at it.
    class Actor
    {
    public:
        /// @param model what to pose, from `loadCreature`, `loadProp` or `buildNpc`. It says which
        ///        animation group to stand in as well as what to stand there with.
        /// @param transform where in the world it stands.
        Actor(World& world, ActorModel model, const osg::Matrixf& transform);
        ~Actor();

        Actor(const Actor&) = delete;
        Actor& operator=(const Actor&) = delete;

        /// Puts the bones where the keyframes have them at `seconds`, and the skin on the bones.
        ///
        /// Wrapped to the played group's own length, so every time is a valid time and a caller
        /// stepping a clock never has to know how long the animation is.
        ///
        /// @param elapsed how much time the world advanced since the last pose, which is what the
        ///        update traversal's frame stamp carries. **Separate from `seconds` because the two
        ///        clocks are different kinds of thing**: an animation is sampled at a time and can
        ///        be scrubbed anywhere in its track, where a particle system integrates and can only
        ///        go forwards. Feeding it the animation's wrapped time runs a plume backwards once
        ///        per loop.
        void pose(float seconds, float elapsed);

        /// How long the group being played runs, in seconds. Zero where there was none to find.
        float getDuration() const { return mStop - mStart; }

        /// How many of the skeleton's bones the keyframes reached.
        ///
        /// **Zero is the failure that looks like success**: a keyframe file whose bone names miss
        /// the skeleton's loads without complaint, attaches nothing, and poses an actor that stands
        /// perfectly still — which is also what a correct actor holding a still frame looks like.
        std::size_t getPosedBones() const { return mPosedBones; }

        /// What was loaded, which is not what was asked for: the `x`-prefixed skeleton beside it.
        const VFS::Path::Normalized& getSkeleton() const { return mModel.mSkeleton; }

        osg::Group& getRoot() const { return *mModel.mRoot; }
        const osg::Matrixf& getTransform() const { return mTransform; }

    private:
        class Clock;

        /// Held rather than made per pose, because the controllers hold a pointer to it.
        std::shared_ptr<Clock> mClock;

        /// What every controller the *model* carries reads, as against the keyframe track hung on
        /// its bones.
        ///
        /// **Two clocks because they measure different things.** A keyframe controller is driven by
        /// which animation is playing and how far into it the actor is, which is `mClock` and is
        /// wrapped to the group's length. Everything a `.nif` carries of its own — the particle
        /// emitter's start and stop window, a scrolling texture, a flicker — is on the world's
        /// clock and only goes forwards. `NifOsg` leaves those sourceless unless the model asked to
        /// autoplay, and a sourceless `ParticleSystemController` freezes its own emitter.
        std::shared_ptr<Clock> mWorldClock;

        /// **A real cull traversal, because that is the only kind a skinned body poses for.** See
        /// `Rtx::PoseCull`; the mirror carries one too, for the actors nothing else reached.
        std::unique_ptr<Rtx::PoseCull> mCull;
        std::unique_ptr<osgUtil::UpdateVisitor> mUpdate;

        /// What the update traversal tells anything that integrates rather than samples — which in
        /// Morrowind's content means the particle emitters and nothing else.
        osg::ref_ptr<osg::FrameStamp> mStamp;

        ActorModel mModel;
        osg::Matrixf mTransform;

        /// The slice of the track being played, in the track's own seconds.
        ///
        /// **A Morrowind keyframe file is every animation the actor has, laid end to end**, and the
        /// text keys are what say where one stops and the next begins. A pose taken at an arbitrary
        /// time in the whole file is a person frozen mid-swing or mid-death — which is exactly what a
        /// crowd looked like before this was here.
        float mStart = 0.0f;
        float mStop = 0.0f;
        std::size_t mPosedBones = 0;

        /// Both traversals are keyed on this and both refuse to run twice on the same number, which
        /// is how OpenMW keeps a frame from skinning an actor once per camera. A pose is a frame as
        /// far as they are concerned, so it counts.
        unsigned int mTraversal = 0;

        /// The particle clock: only ever forwards, whatever the animation's does.
        double mIntegrated = 0.0;

        /// The most an emitter may be asked to integrate in one pose, in seconds.
        ///
        /// A tenth is several frames at any rate worth looking at, and it is shorter than the
        /// lifetime of anything the game emits — so a window that stalled resumes with its plumes
        /// where it left them rather than with a frame's worth of every particle at once.
        static constexpr float sLongestStep = 0.1f;
    };

    /// Where the `index`th of `count` actors stands in front of a camera, facing it.
    ///
    /// **Aimed rather than given.** An actor is put into a shot to be looked at, and the one place
    /// it is certain to be looked at is where the camera already points — so it goes at the target,
    /// and several of them go in a row across the view. Standing them upright whatever the camera's
    /// pitch, because a creature leaning back at the angle you happened to look down from is a
    /// picture of the harness rather than of the creature.
    osg::Matrixf placeActor(const osg::Vec3f& origin, const osg::Vec3f& target, std::size_t index, std::size_t count);
}

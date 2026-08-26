#include "actor.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include <osg/FrameStamp>
#include <osg/Group>
#include <osgUtil/CullVisitor>
#include <osgUtil/UpdateVisitor>

#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/rtx/posecull.hpp>
#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/keyframe.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/vfs/manager.hpp>

#include "world.hpp"

namespace RtxTool
{
    /// The one number every keyframe controller on this actor reads.
    class Actor::Clock : public SceneUtil::ControllerSource
    {
    public:
        float mSeconds = 0.0f;

        float getValue(osg::NodeVisitor*) override { return mSeconds; }
    };

    namespace
    {
        /// The group everything falls back to: somebody standing still with nothing in their hands.
        constexpr std::string_view sPlainIdle = "idle";

        /// Where the animation for `model` lives, by Morrowind's own convention.
        ///
        /// `correctActorModelPath` has already turned `cliffracer.nif` into `xcliffracer.nif`; the
        /// keyframes sit beside it under the same name.
        VFS::Path::Normalized keyframesFor(VFS::Path::NormalizedView skeleton)
        {
            VFS::Path::Normalized keyframes(skeleton);
            keyframes.changeExtension(VFS::Path::ExtensionView("kf"));
            return keyframes;
        }
    }

    ActorModel loadCreature(World& world, VFS::Path::NormalizedView model)
    {
        Resource::ResourceSystem& resources = world.getResourceSystem();

        ActorModel loaded;
        loaded.mSkeleton = Misc::ResourceHelpers::correctActorModelPath(model, resources.getVFS());

        // An instance and not a template: keyframe controllers are about to be attached to its
        // nodes, and a template is shared with every other reference to the same model.
        osg::ref_ptr<osg::Node> created = resources.getSceneManager()->getInstance(loaded.mSkeleton);

        // A model with no skinning in it loads as a plain group, and one wrapped in a skeleton it
        // has no bones for is still worth placing — the pose simply does nothing to it.
        loaded.mRoot = dynamic_cast<SceneUtil::Skeleton*>(created.get());
        if (loaded.mRoot == nullptr)
        {
            osg::ref_ptr<SceneUtil::Skeleton> wrapper = new SceneUtil::Skeleton;
            wrapper->addChild(created);
            loaded.mRoot = wrapper;
        }

        return loaded;
    }

    ActorModel loadProp(World& world, VFS::Path::NormalizedView model)
    {
        ActorModel loaded;
        loaded.mSkeleton = VFS::Path::Normalized(model);

        // Wrapped for the reason `loadCreature` wraps a model with no skinning in it: `Actor` poses
        // a `SceneUtil::Skeleton`, and one with no bones simply passes the traversal through.
        osg::ref_ptr<SceneUtil::Skeleton> wrapper = new SceneUtil::Skeleton;
        wrapper->addChild(world.getResourceSystem().getSceneManager()->getInstance(loaded.mSkeleton));
        loaded.mRoot = wrapper;

        return loaded;
    }

    namespace
    {
        /// Whether `key` is `group` followed by exactly `suffix`.
        bool names(std::string_view key, std::string_view group, std::string_view suffix)
        {
            return key.size() == group.size() + suffix.size() && key.starts_with(group) && key.ends_with(suffix);
        }

        /// The slice of the track that `group` occupies, or nothing where there is no such group.
        ///
        /// **The last start and the last stop**, which is the game's own choice and not an oversight:
        /// Morrowind ships keyframe files with a group marked twice, and the later marker is the one
        /// its own playback uses.
        std::optional<Span> playedRange(const SceneUtil::TextKeyMap& keys, std::string_view group, float whole)
        {
            Span played{ .mStart = 0.0f, .mStop = whole };
            bool found = false;

            for (const auto& [when, key] : keys)
            {
                if (names(key, group, ": start") || names(key, group, ": loop start"))
                {
                    played.mStart = when;
                    played.mStop = whole;
                    found = true;
                }
                else if (found && (names(key, group, ": stop") || names(key, group, ": loop stop")))
                    played.mStop = when;
            }

            return found ? std::optional<Span>(played) : std::nullopt;
        }
    }

    Actor::Actor(World& world, ActorModel model, const osg::Matrixf& transform)
        : mClock(std::make_shared<Clock>())
        , mWorldClock(std::make_shared<Clock>())
        , mCull(std::make_unique<Rtx::PoseCull>())
        , mUpdate(std::make_unique<osgUtil::UpdateVisitor>())
        , mStamp(new osg::FrameStamp)
        , mModel(std::move(model))
        , mTransform(transform)
    {
        // **Both traversals, because a controller can hang off either.** A keyframe or an emitter
        // is driven under update; `NifOsg`'s state-set controllers — a scrolling texture, a
        // flicker — are applied under cull, and `SceneUtil::FrameTimeSource` reads the visitor's
        // frame stamp without checking that it has one.
        mUpdate->setFrameStamp(mStamp);
        mCull->setFrameStamp(mStamp);

        // **Before the keyframes go on, and it never overwrites one.** The game does this through
        // `MWRender::Animation`; without it every controller `NifOsg` left sourceless does nothing,
        // and a `ParticleSystemController` with no source does worse than nothing — it freezes the
        // emitter it is attached to, so a candle keeps the handful of specks its file was saved
        // with and never lights.
        SceneUtil::AssignControllerSourcesVisitor sourced(mWorldClock);
        mModel.mRoot->accept(sourced);

        Resource::ResourceSystem& resources = world.getResourceSystem();

        // **Silent, because most of what comes through here has none.** A prop is posed by this too
        // and no static in the game ships a keyframe file, so a line per model is a line per candle.
        // What says a *creature* failed to find its animation is `getPosedBones` coming back zero.
        const VFS::Path::Normalized keyframes = keyframesFor(mModel.mSkeleton);
        if (!resources.getVFS()->exists(keyframes))
            return;

        osg::ref_ptr<const SceneUtil::KeyframeHolder> track = resources.getKeyframeManager()->get(keyframes);
        if (track == nullptr)
            return;

        SceneUtil::NodeMap bones;
        SceneUtil::NodeMapVisitor collect(bones);
        mModel.mRoot->accept(collect);

        float whole = 0.0f;

        for (const auto& [name, controller] : track->mKeyframeControllers)
        {
            const auto bone = bones.find(Misc::StringUtils::lowerCase(name));
            if (bone == bones.end())
                continue;

            // Cloned because the source is what makes a controller answer, and the manager's copy is
            // shared with every other actor of this kind. Shallow, because the keys behind it are
            // the same keys and are read and not written.
            osg::ref_ptr<SceneUtil::KeyframeController> posed = osg::clone(controller.get(), osg::CopyOp::SHALLOW_COPY);
            posed->setSource(mClock);
            bone->second->addUpdateCallback(posed->getAsCallback());

            if (const std::shared_ptr<SceneUtil::ControllerFunction> function = posed->getFunction())
                whole = std::max(whole, function->getMaximum());

            ++mPosedBones;
        }

        // **The stance, then what stands in for it, then somebody standing there.** Vanilla's base
        // animation has four of the twelve weapon idles in it, so the second of these is the
        // ordinary answer and not a rescue. The last resort is the whole file, which is every
        // animation the actor owns laid end to end — right for a creature with one continuous
        // track and wrong for anything with text keys, which is why it is last.
        std::optional<Span> played;
        for (const std::string_view stance :
            { std::string_view(mModel.mIdle), std::string_view(mModel.mIdleFallback), sPlainIdle })
        {
            played = playedRange(track->mTextKeys, stance, whole);
            if (played.has_value())
                break;
        }

        mStart = played.has_value() ? played->mStart : 0.0f;
        mStop = played.has_value() ? played->mStop : whole;
    }

    // Out of line because the members it destroys are only forward declared in the header.
    Actor::~Actor() = default;

    osg::Matrixf placeActor(const osg::Vec3f& origin, const osg::Vec3f& target, std::size_t index, std::size_t count)
    {
        /// Wide enough that two of the game's largest creatures stand clear of one another.
        constexpr float sSpacing = 200.0f;

        osg::Vec3f forward = target - origin;
        forward.z() = 0.0f;
        if (forward.normalize() == 0.0f)
            forward = osg::Vec3f(0.0f, 1.0f, 0.0f);

        const osg::Vec3f across(forward.y(), -forward.x(), 0.0f);
        const float offset = (static_cast<float>(index) - static_cast<float>(count - 1) * 0.5f) * sSpacing;

        // Morrowind models its actors facing +Y, so turning +Y round to the way back to the eye is
        // what makes one look at whoever is looking at it.
        osg::Matrixf transform = osg::Matrixf::rotate(osg::Vec3f(0.0f, 1.0f, 0.0f), -forward);
        transform.setTrans(target + across * offset);
        return transform;
    }

    void Actor::pose(float seconds, float elapsed)
    {
        const float length = getDuration();
        mClock->mSeconds = length > 0.0f ? mStart + std::fmod(std::fmod(seconds, length) + length, length) : mStart;

        // **Only ever forwards, and never by more than a frame or two.** An emitter integrates the
        // difference between one frame stamp and the last, so a step of nothing simulates nothing
        // and a step of a hundred seconds teleports every plume in the cell to its own ceiling —
        // which is what a window paused on a breakpoint and resumed would otherwise hand it.
        mIntegrated += static_cast<double>(std::clamp(elapsed, 0.0f, sLongestStep));
        mStamp->setSimulationTime(mIntegrated);
        mStamp->setReferenceTime(mIntegrated);
        mWorldClock->mSeconds = static_cast<float>(mIntegrated);

        // **Update then cull, and never the same number twice.** The bones move under the update
        // traversal and the skin follows them under the cull; both keep the last number they saw and
        // return early on a repeat, which in the game stops a second camera skinning the same actor
        // again and here would silently stop a pose from happening at all.
        ++mTraversal;
        mStamp->setFrameNumber(mTraversal);

        mUpdate->setTraversalNumber(mTraversal);
        mModel.mRoot->accept(*mUpdate);

        mCull->setTraversalNumber(mTraversal);
        mModel.mRoot->accept(*mCull);
    }
}

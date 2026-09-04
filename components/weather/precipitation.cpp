#include "precipitation.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include <osg/Group>
#include <osg/PositionAttitudeTransform>
#include <osg/Texture2D>
#include <osg/Transform>
#include <osgParticle/BoxPlacer>
#include <osgParticle/ConstantRateCounter>
#include <osgParticle/ModularEmitter>
#include <osgParticle/ModularProgram>
#include <osgParticle/Operator>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>
#include <osgParticle/Shooter>

#include <components/fallback/fallback.hpp>
#include <components/nifosg/particle.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/settings/values.hpp>
#include <components/surface/material.hpp>

namespace Weather
{
    /// Emits at the rate the weather asks for, with a cap on how far one step may jump.
    class RainCounter : public osgParticle::ConstantRateCounter
    {
    public:
        int numParticlesToCreate(double dt) const override
        {
            // limit dt to avoid large particle emissions if there are jumps in the simulation time
            // 0.2 seconds is the same cap as used in Engine's frame loop
            dt = std::min(dt, 0.2);
            return ConstantRateCounter::numParticlesToCreate(dt);
        }
    };

    /// Sends a drop down at the angle the wind has leant it to.
    class RainShooter : public osgParticle::Shooter
    {
    public:
        RainShooter() = default;

        osg::Object* cloneType() const override { return new RainShooter; }

        osg::Object* clone(const osg::CopyOp&) const override { return new RainShooter(*this); }

        void shoot(osgParticle::Particle* particle) const override
        {
            particle->setVelocity(mVelocity);
            particle->setAngle(osg::Vec3f(-mAngle, 0.f, 0.f));
        }

        void setVelocity(const osg::Vec3f& velocity) { mVelocity = velocity; }
        void setAngle(float angle) { mAngle = angle; }

    private:
        osg::Vec3f mVelocity;
        float mAngle = 0.f;
    };

    namespace
    {
        /// Root first, because a transform lower down multiplies onto what its parents already
        /// carried — which is the order `osg::computeLocalToWorld` walks a path in.
        void accumulateLocalToWorld(const osg::Node& node, osg::Matrix& into)
        {
            if (node.getNumParents() > 0)
                accumulateLocalToWorld(*node.getParent(0), into);

            if (const osg::Transform* transform = node.asTransform())
                transform->computeLocalToWorldMatrix(into, nullptr);
        }

        /// Carries a particle that has left the box back in at the far side, and slides the whole
        /// box along with the eye — which is what makes a finite handful of drops a whole rainstorm.
        class WrapAroundOperator : public osgParticle::Operator
        {
        public:
            WrapAroundOperator(const osg::Vec3f& eye, const osg::Vec3& wrapRange)
                : osgParticle::Operator()
                , mEye(eye)
                , mWrapRange(wrapRange)
                , mHalfWrapRange(mWrapRange / 2.0)
                , mPreviousCameraPosition(eye)
            {
            }

            osg::Object* cloneType() const override { return nullptr; }

            osg::Object* clone(const osg::CopyOp& op) const override { return nullptr; }

            void operate(osgParticle::Particle* particle, double dt) override {}

            void operateParticles(osgParticle::ParticleSystem* ps, double dt) override
            {
                osg::Vec3 position = getCameraPosition();
                osg::Vec3 positionDifference = position - mPreviousCameraPosition;

                // **The box travels with the eye, so where the eye stands is not part of the frame
                // the box is defined in.** The placer fills `±range/2` about the origin of whatever
                // the particles hang under, and the wrap below is about that origin too — so a
                // parent carrying a translation would have every drop wrapped about the middle of
                // the world instead. Upstream never had to say this: its only parent is
                // `CameraRelativeTransform`, which zeroes its own translation for exactly this
                // reason. This fork hangs the same systems under a plain transform at the eye as
                // well, and there the drops drifted out of the box towards the origin until only the
                // newest were anywhere near the camera.
                osg::Matrix toWorld = localToWorldOf(*ps);
                toWorld.setTrans(osg::Vec3f(0.f, 0.f, 0.f));

                osg::Matrix toLocal;
                toLocal.invert(toWorld);

                for (int i = 0; i < ps->numParticles(); ++i)
                {
                    osgParticle::Particle* p = ps->getParticle(i);
                    p->setPosition(toWorld.preMult(p->getPosition()));
                    p->setPosition(p->getPosition() - positionDifference);

                    for (int j = 0; j < 3; ++j) // wrap-around in all 3 dimensions
                    {
                        osg::Vec3 pos = p->getPosition();

                        if (pos[j] < -mHalfWrapRange[j])
                            pos[j] = mHalfWrapRange[j] + fmod(pos[j] - mHalfWrapRange[j], mWrapRange[j]);
                        else if (pos[j] > mHalfWrapRange[j])
                            pos[j] = fmod(pos[j] + mHalfWrapRange[j], mWrapRange[j]) - mHalfWrapRange[j];

                        p->setPosition(pos);
                    }

                    p->setPosition(toLocal.preMult(p->getPosition()));
                }

                mPreviousCameraPosition = position;
            }

        protected:
            /// The owner's, so that moving the eye needs no reaching back in here.
            const osg::Vec3f& mEye;
            osg::Vec3 mWrapRange;
            osg::Vec3 mHalfWrapRange;
            osg::Vec3 mPreviousCameraPosition;

            osg::Vec3 getCameraPosition() { return mEye; }
        };

        /// Fades every particle of a system together, as the weather comes and goes.
        class WeatherAlphaOperator : public osgParticle::Operator
        {
        public:
            WeatherAlphaOperator(float& alpha, bool rain)
                : mAlpha(alpha)
                , mIsRain(rain)
            {
            }

            osg::Object* cloneType() const override { return nullptr; }

            osg::Object* clone(const osg::CopyOp& op) const override { return nullptr; }

            void operate(osgParticle::Particle* particle, double dt) override
            {
                constexpr float rainThreshold = 0.6f; // Rain_Threshold?
                float alpha = mIsRain ? mAlpha * rainThreshold : mAlpha;
                particle->setAlphaRange(osgParticle::rangef(alpha, alpha));
            }

        private:
            float& mAlpha;
            bool mIsRain;
        };

        // Updater for alpha value on a node's StateSet. Assumes the node has an existing Material
        // StateAttribute.
        class AlphaFader : public SceneUtil::StateSetUpdater
        {
        public:
            /// @param alpha the variable alpha value is recovered from
            AlphaFader(const float& alpha)
                : mAlpha(alpha)
            {
            }

            void setDefaults(osg::StateSet* stateset) override
            {
                // need to create a deep copy of StateAttributes we will modify
                SceneUtil::Material* mat
                    = static_cast<SceneUtil::Material*>(stateset->getAttribute(osg::StateAttribute::MATERIAL));
                stateset->setAttribute(osg::clone(mat, osg::CopyOp::DEEP_COPY_ALL), osg::StateAttribute::ON);
            }

            void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override
            {
                SceneUtil::Material* mat
                    = static_cast<SceneUtil::Material*>(stateset->getAttribute(osg::StateAttribute::MATERIAL));
                mat->setDiffuse(osg::Vec4f(0.f, 0.f, 0.f, mAlpha));
            }

        protected:
            const float& mAlpha;
        };

        // Helper for adding AlphaFaders to a subgraph
        class SetupVisitor : public osg::NodeVisitor
        {
        public:
            SetupVisitor(const float& alpha)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mAlpha(alpha)
            {
            }

            void apply(osg::Node& node) override
            {
                if (osg::StateSet* stateset = node.getStateSet())
                {
                    if (stateset->getAttribute(osg::StateAttribute::MATERIAL))
                    {
                        SceneUtil::CompositeStateSetUpdater* composite = nullptr;
                        osg::Callback* callback = node.getUpdateCallback();

                        while (callback)
                        {
                            composite = dynamic_cast<SceneUtil::CompositeStateSetUpdater*>(callback);
                            if (composite)
                                break;

                            callback = callback->getNestedCallback();
                        }

                        osg::ref_ptr<AlphaFader> alphaFader = new AlphaFader(mAlpha);

                        if (composite)
                            composite->addController(alphaFader);
                        else
                            node.addUpdateCallback(alphaFader);
                    }
                }

                traverse(node);
            }

        private:
            const float& mAlpha;
        };

        /// How far a driven effect's particles run before the wrap carries them back.
        const osg::Vec3f sEffectWrapRange(1024, 1024, 800);
    }

    osg::Matrix localToWorldOf(const osg::Node& node)
    {
        osg::Matrix matrix;
        accumulateLocalToWorld(node, matrix);
        return matrix;
    }

    Precipitation::Precipitation(osg::Group* parent, Resource::SceneManager& scenes, osg::Node::NodeMask mask)
        : mSceneManager(scenes)
        , mMask(mask)
        , mRainRipplesEnabled(Fallback::Map::getBool("Weather_Rain_Ripples"))
        , mSnowRipplesEnabled(Fallback::Map::getBool("Weather_Snow_Ripples"))
        , mStormDirection(defaultStormDirection())
    {
        mNode = new osg::Group;
        mNode->setName("Precipitation");
        parent->addChild(mNode);
    }

    Precipitation::~Precipitation() = default;

    void Precipitation::createRain()
    {
        if (mRainNode)
            return;

        mRainNode = new osg::Group;

        mRainParticleSystem = new NifOsg::ParticleSystem;
        osg::Vec3 rainRange = osg::Vec3(mRainDiameter, mRainDiameter, (mRainMinHeight + mRainMaxHeight) / 2.f);

        mRainParticleSystem->setParticleAlignment(osgParticle::ParticleSystem::FIXED);
        // Vertical placement with some horizontal compression.
        // Z-down alignment is used so that the UV uses Y-down convention
        mRainParticleSystem->setAlignVectors(osg::Vec3f(0.1f, 0, 0), osg::Vec3f(0, 0, -1.f));

        osg::ref_ptr<osg::StateSet> stateset = mRainParticleSystem->getOrCreateStateSet();

        constexpr VFS::Path::NormalizedView raindropImage("textures/tx_raindrop_01.dds");
        osg::ref_ptr<osg::Texture2D> raindropTex
            = new osg::Texture2D(mSceneManager.getImageManager()->getImage(raindropImage));
        raindropTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        raindropTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

        stateset->setTextureAttribute(0, raindropTex);
        stateset->setNestRenderBins(false);
        stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
        stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
        stateset->setMode(GL_BLEND, osg::StateAttribute::ON);

        osg::ref_ptr<SceneUtil::Material> mat = new SceneUtil::Material;
        mat->setAmbient(osg::Vec4f(1, 1, 1, 1));
        mat->setDiffuse(osg::Vec4f(1, 1, 1, 1));
        mat->setVertexColorMode(SceneUtil::VertexColorModes::AmbientAndDiffuse);
        stateset->setAttributeAndModes(mat);

        // **Said in `Surface`'s terms as well as OpenGL's, because this is the one drop of rain
        // nothing loaded from a file.** Everything else the weather throws comes out of a NIF, and
        // the content pipeline describes what it builds; this state set is assembled here, so
        // `ShaderVisitor` finds nothing to augment — it augments a description and does not invent
        // one — and a renderer that reads the description rather than the attribute finds a
        // particle system with no material at all. The alpha mode is what the blend above says.
        Surface::Material described;
        described.setTexture(Surface::TextureRole::Diffuse, raindropTex->getImage());
        described.mAlphaMode = Surface::AlphaMode::Blend;
        Surface::setMaterial(*stateset, described);

        osgParticle::Particle& particleTemplate = mRainParticleSystem->getDefaultParticleTemplate();
        particleTemplate.setSizeRange(osgParticle::rangef(5.f, 15.f));
        particleTemplate.setAlphaRange(osgParticle::rangef(1.f, 1.f));
        particleTemplate.setLifeTime(1);

        osg::ref_ptr<osgParticle::ModularEmitter> emitter = new osgParticle::ModularEmitter;
        emitter->setParticleSystem(mRainParticleSystem);

        osg::ref_ptr<osgParticle::BoxPlacer> placer = new osgParticle::BoxPlacer;
        placer->setXRange(-rainRange.x() / 2, rainRange.x() / 2);
        placer->setYRange(-rainRange.y() / 2, rainRange.y() / 2);
        placer->setZRange(-rainRange.z() / 2, rainRange.z() / 2);
        emitter->setPlacer(placer);
        mPlacer = placer;

        // FIXME: vanilla engine does not use a particle system to handle rain, it uses a NIF-file with 20 raindrops in
        // it. It spawns the (maxRaindrops-getParticleSystem()->numParticles())*dt/rainEntranceSpeed batches every frame
        // (near 1-2). Since the rain is a regular geometry, it produces water ripples, also in theory it can be removed
        // if collides with something.
        osg::ref_ptr<RainCounter> counter = new RainCounter;
        counter->setNumberOfParticlesPerSecondToCreate(mRainMaxRaindrops / mRainEntranceSpeed * 20);
        emitter->setCounter(counter);
        mCounter = counter;

        osg::ref_ptr<RainShooter> shooter = new RainShooter;
        mRainShooter = shooter;
        emitter->setShooter(shooter);

        osg::ref_ptr<osgParticle::ParticleSystemUpdater> updater = new osgParticle::ParticleSystemUpdater;
        updater->addParticleSystem(mRainParticleSystem);

        osg::ref_ptr<osgParticle::ModularProgram> program = new osgParticle::ModularProgram;
        program->addOperator(new WrapAroundOperator(mEye, rainRange));
        program->addOperator(new WeatherAlphaOperator(mPrecipitationAlpha, true));
        program->setParticleSystem(mRainParticleSystem);
        mRainNode->addChild(program);

        mRainNode->addChild(emitter);

        // **Above the system it drives, which is where `NifOsg` puts it and for the same reason.**
        // A walk that meets the updater first reads a system that has already been integrated this
        // frame; the other way round it reads one frame of staleness into every drop. The
        // rasterizer cannot tell — it bins the drawable here and draws it long afterwards — so this
        // is free there and correct here.
        mRainNode->addChild(updater);
        mRainNode->addChild(mRainParticleSystem);

        mRainNode->setNodeMask(mMask);

        mNode->addChild(mRainNode);
        ++mRevision;
    }

    void Precipitation::destroyRain()
    {
        if (!mRainNode)
            return;

        mNode->removeChild(mRainNode);
        mRainNode = nullptr;
        mPlacer = nullptr;
        mCounter = nullptr;
        mRainParticleSystem = nullptr;
        mRainShooter = nullptr;
        ++mRevision;
    }

    void Precipitation::updateRainParameters()
    {
        if (!mRainShooter)
            return;

        float angle = -std::atan(mWindSpeed / 50.f);
        mRainShooter->setVelocity(osg::Vec3f(0, mRainSpeed * std::sin(angle), -mRainSpeed / std::cos(angle)));
        mRainShooter->setAngle(angle);

        const osg::Vec3f rainRange = getWrapRange();

        mPlacer->setXRange(-rainRange.x() / 2, rainRange.x() / 2);
        mPlacer->setYRange(-rainRange.y() / 2, rainRange.y() / 2);
        mPlacer->setZRange(-rainRange.z() / 2, rainRange.z() / 2);

        mCounter->setNumberOfParticlesPerSecondToCreate(mRainMaxRaindrops / mRainEntranceSpeed * 20);
    }

    osg::Vec3f Precipitation::getWrapRange() const
    {
        if (mRainNode)
            return osg::Vec3f(mRainDiameter, mRainDiameter, (mRainMinHeight + mRainMaxHeight) / 2.f);

        return sEffectWrapRange;
    }

    bool Precipitation::wantsOcclusion() const
    {
        return !mRainEffect.empty() || mCurrentParticleEffect == Settings::models().mWeathersnow.get();
    }

    bool Precipitation::ripplesEnabled() const
    {
        if (hasRain())
            return mRainRipplesEnabled;

        if (mParticleNode && mCurrentParticleEffect == Settings::models().mWeathersnow.get())
            return mSnowRipplesEnabled;

        return false;
    }

    void Precipitation::setWeather(const Downpour& weather)
    {
        mRainEntranceSpeed = weather.mRainEntranceSpeed;
        mRainMaxRaindrops = weather.mRainMaxRaindrops;
        mRainDiameter = weather.mRainDiameter;
        mRainMinHeight = weather.mRainMinHeight;
        mRainMaxHeight = weather.mRainMaxHeight;
        mRainSpeed = weather.mRainSpeed;
        mWindSpeed = weather.mWindSpeed;
        mBaseWindSpeed = weather.mBaseWindSpeed;
        mPrecipitationAlpha = weather.mPrecipitationAlpha;
        mIsStorm = weather.mIsStorm;

        if (mRainEffect != weather.mRainEffect)
        {
            mRainEffect = weather.mRainEffect;
            if (!mRainEffect.empty())
                createRain();
            else
                destroyRain();
        }

        updateRainParameters();

        if (mCurrentParticleEffect == weather.mParticleEffect)
            return;

        mCurrentParticleEffect = weather.mParticleEffect;

        // cleanup old particles
        if (mParticleEffect)
        {
            mParticleNode->removeChild(mParticleEffect);
            mParticleEffect = nullptr;
        }

        if (mCurrentParticleEffect.empty())
        {
            if (mParticleNode)
            {
                mNode->removeChild(mParticleNode);
                mParticleNode = nullptr;
            }

            ++mRevision;
            return;
        }

        if (!mParticleNode)
        {
            mParticleNode = new osg::PositionAttitudeTransform;
            mParticleNode->setNodeMask(mMask);
            mNode->addChild(mParticleNode);
        }

        mParticleEffect = mSceneManager.getInstance(mCurrentParticleEffect, mParticleNode);

        SceneUtil::AssignControllerSourcesVisitor assignVisitor(std::make_shared<SceneUtil::FrameTimeSource>());
        mParticleEffect->accept(assignVisitor);

        SetupVisitor alphaFaderSetupVisitor(mPrecipitationAlpha);
        mParticleEffect->accept(alphaFaderSetupVisitor);

        SceneUtil::FindByClassVisitor findPSVisitor("ParticleSystem");
        mParticleEffect->accept(findPSVisitor);

        const bool occluded = wantsOcclusion();

        for (unsigned int i = 0; i < findPSVisitor.mFoundNodes.size(); ++i)
        {
            osgParticle::ParticleSystem* ps = static_cast<osgParticle::ParticleSystem*>(findPSVisitor.mFoundNodes[i]);

            osg::ref_ptr<osgParticle::ModularProgram> program = new osgParticle::ModularProgram;
            if (occluded)
                program->addOperator(new WrapAroundOperator(mEye, sEffectWrapRange));
            program->addOperator(new WeatherAlphaOperator(mPrecipitationAlpha, false));
            program->setParticleSystem(ps);
            mParticleNode->addChild(program);

            for (int particleIndex = 0; particleIndex < ps->numParticles(); ++particleIndex)
            {
                ps->getParticle(particleIndex)
                    ->setAlphaRange(osgParticle::rangef(mPrecipitationAlpha, mPrecipitationAlpha));
                ps->getParticle(particleIndex)->update(0, true);
            }
        }

        ++mRevision;
    }

    void Precipitation::update(const Conditions& where)
    {
        mEye = where.mEye;
        mUnderwater = where.mUnderwater;
        mStormDirection = where.mStormDirection;

        // Held where they are rather than hidden: what stops being *drawn* is the renderer's to
        // decide, and it is the renderer that said so in the first place.
        if (mRainParticleSystem)
            mRainParticleSystem->setFrozen(mUnderwater);

        if (!mIsStorm || !mParticleNode)
            return;

        osg::Quat quat;
        quat.makeRotate(defaultStormDirection(), mStormDirection);
        // Morrowind deliberately rotates the blizzard mesh, so so should we.
        if (mCurrentParticleEffect == Settings::models().mWeatherblizzard.get())
            quat.makeRotate(osg::Vec3f(-1, 0, 0), mStormDirection);

        mParticleNode->setAttitude(quat);
    }
}

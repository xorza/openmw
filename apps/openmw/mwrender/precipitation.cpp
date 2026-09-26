#include "precipitation.hpp"

#include <cmath>
#include <memory>

#include <osg/Group>
#include <osg/PositionAttitudeTransform>
#include <osg/Texture2D>
#include <osgParticle/BoxPlacer>
#include <osgParticle/ModularEmitter>
#include <osgParticle/ModularProgram>
#include <osgParticle/Operator>
#include <osgParticle/ParticleSystemUpdater>

#include <components/fallback/fallback.hpp>
#include <components/nifosg/particle.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/settings/values.hpp>

#include "../mwworld/weather.hpp"
#include "vismask.hpp"

// Upstream's, from sky.cpp.
namespace
{
    class WrapAroundOperator : public osgParticle::Operator
    {
    public:
        WrapAroundOperator(osg::Camera* camera, const osg::Vec3& wrapRange)
            : osgParticle::Operator()
            , mCamera(camera)
            , mWrapRange(wrapRange)
            , mHalfWrapRange(mWrapRange / 2.0)
        {
            mPreviousCameraPosition = getCameraPosition();
        }

        osg::Object* cloneType() const override { return nullptr; }

        osg::Object* clone(const osg::CopyOp& op) const override { return nullptr; }

        void operate(osgParticle::Particle* particle, double dt) override {}

        void operateParticles(osgParticle::ParticleSystem* ps, double dt) override
        {
            osg::Vec3 position = getCameraPosition();
            osg::Vec3 positionDifference = position - mPreviousCameraPosition;

            osg::Matrix toWorld, toLocal;

            std::vector<osg::Matrix> worldMatrices = ps->getWorldMatrices();

            if (!worldMatrices.empty())
            {
                toWorld = worldMatrices[0];
                toLocal.invert(toWorld);
            }

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
        osg::Camera* mCamera;
        osg::Vec3 mPreviousCameraPosition;
        osg::Vec3 mWrapRange;
        osg::Vec3 mHalfWrapRange;

        osg::Vec3 getCameraPosition() { return mCamera->getInverseViewMatrix().getTrans(); }
    };

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

    // Updater for alpha value on a node's StateSet. Assumes the node has an existing Material StateAttribute.
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
}

namespace MWRender
{
    Precipitation::Precipitation(osg::Group* parentNode, osg::Camera* camera, Resource::SceneManager* sceneManager)
        : mSceneManager(sceneManager)
        , mCamera(camera)
        , mIsStorm(false)
        , mStormParticleDirection(MWWorld::Weather::defaultDirection())
        , mRainSpeed(0.f)
        , mRainDiameter(0.f)
        , mRainMinHeight(0.f)
        , mRainMaxHeight(0.f)
        , mRainEntranceSpeed(1.f)
        , mRainMaxRaindrops(0)
        , mRainRipplesEnabled(Fallback::Map::getBool("Weather_Rain_Ripples"))
        , mSnowRipplesEnabled(Fallback::Map::getBool("Weather_Snow_Ripples"))
        , mWindSpeed(0.f)
        , mEnabled(true)
        , mPrecipitationAlpha(0.f)
        , mDirtyParticlesEffect(false)
    {
        mRoot = new CameraRelativeTransform;
        mRoot->setName("Precipitation Root");
        mRoot->setNodeMask(Mask_Sky);
        parentNode->addChild(mRoot);

        mUnderwaterSwitch = new UnderwaterSwitchCallback(mRoot);
    }

    Precipitation::~Precipitation()
    {
        if (mRoot)
        {
            mRoot->getParent(0)->removeChild(mRoot);
            mRoot = nullptr;
        }
    }

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
            = new osg::Texture2D(mSceneManager->getImageManager()->getImage(raindropImage));
        raindropTex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        raindropTex->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);

        stateset->setTextureAttribute(0, raindropTex);
        // Named, so a renderer that reads what a surface is finds the drop's texture
        stateset->setTextureAttribute(0, new SceneUtil::TextureType("diffuseMap"), osg::StateAttribute::ON);
        stateset->setNestRenderBins(false);
        stateset->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
        stateset->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
        stateset->setMode(GL_BLEND, osg::StateAttribute::ON);

        osg::ref_ptr<SceneUtil::Material> mat = new SceneUtil::Material;
        mat->setAmbient(osg::Vec4f(1, 1, 1, 1));
        mat->setDiffuse(osg::Vec4f(1, 1, 1, 1));
        mat->setVertexColorMode(SceneUtil::VertexColorModes::AmbientAndDiffuse);
        stateset->setAttributeAndModes(mat);

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
        program->addOperator(new WrapAroundOperator(mCamera, rainRange));
        program->addOperator(new WeatherAlphaOperator(mPrecipitationAlpha, true));
        program->setParticleSystem(mRainParticleSystem);
        mRainNode->addChild(program);

        mRainNode->addChild(emitter);
        mRainNode->addChild(mRainParticleSystem);
        mRainNode->addChild(updater);

        // Note: if we ever switch to regular geometry rain, it'll need to use an AlphaFader.
        mRainNode->addCullCallback(mUnderwaterSwitch);
        mRainNode->setNodeMask(Mask_WeatherParticles);

        mRainParticleSystem->setUserValue("simpleLighting", true);
        mRainParticleSystem->setUserValue("particleOcclusion", true);
        mSceneManager->recreateShaders(mRainNode);

        mRoot->addChild(mRainNode);
        mOccluded = true;
    }

    void Precipitation::destroyRain()
    {
        if (!mRainNode)
            return;

        mRoot->removeChild(mRainNode);
        mRainNode = nullptr;
        mPlacer = nullptr;
        mCounter = nullptr;
        mRainParticleSystem = nullptr;
        mRainShooter = nullptr;
        mOccluded = false;
    }

    bool Precipitation::hasRain() const
    {
        return mRainNode != nullptr;
    }

    bool Precipitation::getRainRipplesEnabled() const
    {
        if (!mEnabled)
            return false;

        if (hasRain())
            return mRainRipplesEnabled;

        if (mParticleNode && mCurrentParticleEffect == Settings::models().mWeathersnow.get())
            return mSnowRipplesEnabled;

        return false;
    }

    float Precipitation::getRainOnWater() const
    {
        return getRainRipplesEnabled() ? mPrecipitationAlpha : 0.0f;
    }

    void Precipitation::update()
    {
        if (!mEnabled)
            return;

        switchUnderwaterRain();

        if (mIsStorm && mParticleNode)
        {
            osg::Quat quat;
            quat.makeRotate(MWWorld::Weather::defaultDirection(), mStormParticleDirection);
            // Morrowind deliberately rotates the blizzard mesh, so so should we.
            if (mCurrentParticleEffect == Settings::models().mWeatherblizzard.get())
                quat.makeRotate(osg::Vec3f(-1, 0, 0), mStormParticleDirection);
            mParticleNode->setAttitude(quat);
        }
    }

    void Precipitation::setEnabled(bool enabled)
    {
        const osg::Node::NodeMask mask = enabled ? Mask_Sky : 0u;

        mRoot->setNodeMask(mask);

        if (!enabled && mParticleNode && mParticleEffect)
        {
            mCurrentParticleEffect.clear();
            mDirtyParticlesEffect = true;
        }

        mEnabled = enabled;
    }

    void Precipitation::updateRainParameters()
    {
        if (mRainShooter)
        {
            float angle = -std::atan(mWindSpeed / 50.f);
            mRainShooter->setVelocity(osg::Vec3f(0, mRainSpeed * std::sin(angle), -mRainSpeed / std::cos(angle)));
            mRainShooter->setAngle(angle);

            osg::Vec3 rainRange = osg::Vec3(mRainDiameter, mRainDiameter, (mRainMinHeight + mRainMaxHeight) / 2.f);

            mPlacer->setXRange(-rainRange.x() / 2, rainRange.x() / 2);
            mPlacer->setYRange(-rainRange.y() / 2, rainRange.y() / 2);
            mPlacer->setZRange(-rainRange.z() / 2, rainRange.z() / 2);

            mCounter->setNumberOfParticlesPerSecondToCreate(mRainMaxRaindrops / mRainEntranceSpeed * 20);
            mOcclusionRange = rainRange;
        }
    }

    void Precipitation::switchUnderwaterRain()
    {
        if (!mRainParticleSystem)
            return;

        bool freeze = mUnderwaterSwitch->isUnderwater();
        mRainParticleSystem->setFrozen(freeze);
    }

    void Precipitation::setWeather(const SkyState& sky)
    {
        const WeatherResult& weather = sky.mWeather;
        mStormParticleDirection = sky.mStormParticleDirection;

        mRainEntranceSpeed = weather.mRainEntranceSpeed;
        mRainMaxRaindrops = weather.mRainMaxRaindrops;
        mRainDiameter = weather.mRainDiameter;
        mRainMinHeight = weather.mRainMinHeight;
        mRainMaxHeight = weather.mRainMaxHeight;
        mRainSpeed = weather.mRainSpeed;
        mWindSpeed = weather.mWindSpeed;

        if (mRainEffect != weather.mRainEffect)
        {
            mRainEffect = weather.mRainEffect;
            if (!mRainEffect.empty())
            {
                createRain();
            }
            else
            {
                destroyRain();
            }
        }

        updateRainParameters();

        mIsStorm = weather.mIsStorm;

        if (mDirtyParticlesEffect || (mCurrentParticleEffect != weather.mParticleEffect))
        {
            mDirtyParticlesEffect = false;
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
                    mRoot->removeChild(mParticleNode);
                    mParticleNode = nullptr;
                }
                if (mRainEffect.empty())
                {
                    mOccluded = false;
                }
            }
            else
            {
                if (!mParticleNode)
                {
                    mParticleNode = new osg::PositionAttitudeTransform;
                    mParticleNode->addCullCallback(mUnderwaterSwitch);
                    mParticleNode->setNodeMask(Mask_WeatherParticles);
                    mRoot->addChild(mParticleNode);
                }

                mParticleEffect = mSceneManager->getInstance(mCurrentParticleEffect, mParticleNode);

                SceneUtil::AssignControllerSourcesVisitor assignVisitor(std::make_shared<SceneUtil::FrameTimeSource>());
                mParticleEffect->accept(assignVisitor);

                SetupVisitor alphaFaderSetupVisitor(mPrecipitationAlpha);
                mParticleEffect->accept(alphaFaderSetupVisitor);

                SceneUtil::FindByClassVisitor findPSVisitor("ParticleSystem");
                mParticleEffect->accept(findPSVisitor);

                const osg::Vec3 defaultWrapRange = osg::Vec3(1024, 1024, 800);
                const bool occlusionEnabledForEffect
                    = !mRainEffect.empty() || mCurrentParticleEffect == Settings::models().mWeathersnow.get();

                for (unsigned int i = 0; i < findPSVisitor.mFoundNodes.size(); ++i)
                {
                    osgParticle::ParticleSystem* ps
                        = static_cast<osgParticle::ParticleSystem*>(findPSVisitor.mFoundNodes[i]);

                    osg::ref_ptr<osgParticle::ModularProgram> program = new osgParticle::ModularProgram;
                    if (occlusionEnabledForEffect)
                        program->addOperator(new WrapAroundOperator(mCamera, defaultWrapRange));
                    program->addOperator(new WeatherAlphaOperator(mPrecipitationAlpha, false));
                    program->setParticleSystem(ps);
                    mParticleNode->addChild(program);

                    for (int particleIndex = 0; particleIndex < ps->numParticles(); ++particleIndex)
                    {
                        ps->getParticle(particleIndex)
                            ->setAlphaRange(osgParticle::rangef(mPrecipitationAlpha, mPrecipitationAlpha));
                        ps->getParticle(particleIndex)->update(0, true);
                    }

                    ps->setUserValue("simpleLighting", true);

                    if (occlusionEnabledForEffect)
                        ps->setUserValue("particleOcclusion", true);
                }

                mSceneManager->recreateShaders(mParticleNode);

                if (occlusionEnabledForEffect)
                {
                    mOccluded = true;
                    mOcclusionRange = defaultWrapRange;
                }
            }
        }

        mPrecipitationAlpha = weather.mPrecipitationAlpha;
    }

    void Precipitation::setWaterHeight(float height)
    {
        mUnderwaterSwitch->setWaterLevel(height);
    }

    void Precipitation::setWaterEnabled(bool enabled)
    {
        mUnderwaterSwitch->setEnabled(enabled);
    }

    void Precipitation::listAssetsToPreload(
        std::vector<VFS::Path::Normalized>& models, std::vector<VFS::Path::Normalized>& textures)
    {
        models.push_back(Settings::models().mWeatherashcloud);
        models.push_back(Settings::models().mWeatherblightcloud);
        models.push_back(Settings::models().mWeathersnow);
        models.push_back(Settings::models().mWeatherblizzard);

        textures.emplace_back("textures/tx_raindrop_01.dds");
    }
}

#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <osg/BlendFunc>
#include <osg/CullFace>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Image>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Sequence>
#include <osg/Switch>
#include <osg/Texture2D>
#include <osg/observer_ptr>
#include <osgParticle/ConstantRateCounter>
#include <osgParticle/ModularEmitter>
#include <osgParticle/Particle>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ParticleSystemUpdater>
#include <osgParticle/RadialShooter>
#include <osgUtil/UpdateVisitor>

#include <components/esm3/loadligh.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/rtx/instancerecord.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/sceneextractor.hpp>
#include <components/rtx/shaders/scene.h>
#include <components/rtx/spritelight.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightcontroller.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/statesetupdater.hpp>
#include <components/sceneutil/vismask.hpp>
#include <components/surface/material.hpp>

namespace Rtx::Testing
{
    inline osg::ref_ptr<osg::Vec3Array> makePositions(std::initializer_list<osg::Vec3f> values)
    {
        osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
        for (const osg::Vec3f& value : values)
            positions->push_back(value);
        return positions;
    }

    inline osg::ref_ptr<osg::DrawElementsUInt> makeTriangles(std::initializer_list<unsigned int> indices)
    {
        osg::ref_ptr<osg::DrawElementsUInt> triangles = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES);
        for (const unsigned int index : indices)
            triangles->push_back(index);
        return triangles;
    }

    /// The description on a state set, created empty where nothing has authored one yet.
    ///
    /// Every fixture here stands in for something `NifOsg` or `Terrain` built, and those author
    /// a `Surface::Material` for everything they build — so a fixture that binds a texture or
    /// sets a colour and describes neither is testing a state the content path cannot produce.
    inline Surface::Material& describe(osg::StateSet& state)
    {
        if (Surface::getMaterial(state) == nullptr)
            Surface::setMaterial(state, Surface::Material{});

        return *Surface::getWritableMaterial(state);
    }

    /// Binds a texture the way a loader does: the unit for the OpenGL renderer's shaders, and
    /// the role for everyone else.
    inline void paint(
        osg::StateSet& state, osg::Image& image, Surface::TextureRole role = Surface::TextureRole::Diffuse)
    {
        state.setTextureAttributeAndModes(0, new osg::Texture2D(&image), osg::StateAttribute::ON);
        describe(state).setTexture(role, &image);
    }

    /// The same for a texture that is nothing but a name, which is all a walk reads of most of
    /// them.
    inline void paint(
        osg::StateSet& state, std::string_view file, Surface::TextureRole role = Surface::TextureRole::Diffuse)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->setFileName(std::string(file));

        paint(state, *image, role);
    }

    /// A unit quad in the xy plane: four vertices, two triangles.
    inline osg::ref_ptr<osg::Geometry> makeQuad()
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        geometry->setVertexArray(makePositions({
            osg::Vec3f(0.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 0.0f, 0.0f),
            osg::Vec3f(1.0f, 1.0f, 0.0f),
            osg::Vec3f(0.0f, 1.0f, 0.0f),
        }));
        geometry->addPrimitiveSet(makeTriangles({ 0, 1, 2, 0, 2, 3 }));
        return geometry;
    }

    /// A light in the graph, placed where the walk found it.
    inline osg::ref_ptr<SceneUtil::LightSource> makeLightSource(
        float radius, const osg::Vec4f& diffuse, const osg::Vec4f& ambient = osg::Vec4f())
    {
        osg::ref_ptr<SceneUtil::Light> light = new SceneUtil::Light;
        light->setDiffuse(diffuse);
        light->setAmbient(ambient);

        osg::ref_ptr<SceneUtil::LightSource> source = new SceneUtil::LightSource;
        source->setRadius(radius);
        source->setLight(light);
        return source;
    }

    /// A skeleton with one bone, and a rig bound rigidly to it.
    ///
    /// Every weight on one bone with an identity bind matrix makes the skinning arithmetic the
    /// bone's own transform and nothing else, so what a test expects is what it moved the bone
    /// by — rather than a fit against whatever a weighted sum happened to produce.
    struct RiggedQuad
    {
        osg::ref_ptr<SceneUtil::Skeleton> mSkeleton = new SceneUtil::Skeleton;
        osg::ref_ptr<osg::MatrixTransform> mBone = new osg::MatrixTransform;
        osg::ref_ptr<SceneUtil::RigGeometry> mRig = new SceneUtil::RigGeometry;
        osg::ref_ptr<osg::Geometry> mSource = makeQuad();

        RiggedQuad()
        {
            mBone->setName("bone");
            mSkeleton->addChild(mBone);

            mRig->setName("shape");
            mRig->setBoneInfo({ SceneUtil::RigGeometry::BoneInfo{
                .mName = "bone", .mBoundSphere = {}, .mInvBindMatrix = osg::Matrixf::identity() } });
            mRig->setInfluences(std::vector<SceneUtil::RigGeometry::BoneWeights>(
                4, SceneUtil::RigGeometry::BoneWeights{ { 0, 1.0f } }));
            mRig->setSourceGeometry(mSource);

            // **A named shape one level below the skeleton, which is the shape NIF content
            // has.** `RigGeometry::updateSkinToSkelMatrix` reads the node path backwards from
            // the trishape's own transform, and a rig hung straight off the skeleton walks off
            // the front of it.
            osg::ref_ptr<osg::Group> holder = new osg::Group;
            holder->addChild(mRig);
            mSkeleton->addChild(holder);
        }

        /// The update traversal the game runs before it mirrors anything.
        ///
        /// **`RigGeometry` finds its skeleton here and nowhere else.** It walks the node path
        /// for one, and the pose traversal the mirror uses is handed the drawable on its own —
        /// so a rig that had never been through an update would have nothing to skin against.
        void update(unsigned int traversal)
        {
            osgUtil::UpdateVisitor visitor;
            visitor.setTraversalNumber(traversal);
            mSkeleton->accept(visitor);
        }
    };

    /// A state-set controller of the shape `NifOsg` builds out of a `NiMaterialColorController`:
    /// it rewrites one attribute every time it is applied, and it hangs from whichever callback
    /// chain the content asked for.
    class ColourController : public SceneUtil::StateSetUpdater
    {
    public:
        float mRed = 0.0f;

        /// A texture the controlled surface wears, for the tests that care what an animated
        /// material is read from. Null leaves the surface untextured, which is what most of them
        /// want.
        osg::ref_ptr<osg::Image> mDiffuse;

        void setDefaults(osg::StateSet* stateset) override
        {
            stateset->setAttribute(new osg::Material, osg::StateAttribute::ON);
            Surface::setMaterial(*stateset, Surface::Material{});

            if (mDiffuse != nullptr)
                paint(*stateset, *mDiffuse);
        }

        void apply(osg::StateSet* stateset, osg::NodeVisitor*) override
        {
            const osg::Vec4f colour(mRed, 0.0f, 0.0f, 1.0f);
            auto* colours = static_cast<osg::Material*>(stateset->getAttribute(osg::StateAttribute::MATERIAL));
            colours->setDiffuse(osg::Material::FRONT_AND_BACK, colour);
            Surface::getWritableMaterial(*stateset)->mDiffuseColour = colour;
        }
    };
}

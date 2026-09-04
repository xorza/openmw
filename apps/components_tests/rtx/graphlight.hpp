#pragma once

#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/sceneutil/lightmanager.hpp>

namespace Rtx::Testing
{
    /// A light as the scene graph carries one: a `SceneUtil::Light` inside a
    /// `SceneUtil::LightSource`, which is the pair a walk reads and the pair a colour test needs
    /// built.
    ///
    /// A radius of nought is what `SceneUtil::LightSource` starts at, so a test about a light's
    /// colour rather than its reach asks for that rather than leaving the field unsaid.
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
}

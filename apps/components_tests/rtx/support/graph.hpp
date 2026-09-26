#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

#include <osg/Array>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/PrimitiveSet>
#include <osg/StateAttribute>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Texture>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <components/rtx/meshtable.hpp>
#include <components/rtx/surface.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/texturetype.hpp>

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

    /// The material attribute on a state set, made where nothing has set one yet.
    ///
    /// Every builder here stands in for something `NifOsg` built, and a walk reads what that built
    /// off the state set — so a colour a test wants read goes on the attribute the loader would
    /// have put it on.
    inline SceneUtil::Material& colours(osg::StateSet& state)
    {
        auto* material = dynamic_cast<SceneUtil::Material*>(state.getAttribute(osg::StateAttribute::MATERIAL));
        if (material == nullptr)
        {
            material = new SceneUtil::Material;
            state.setAttribute(material, osg::StateAttribute::ON);
        }

        return *material;
    }

    /// Binds a texture the way a loader does: on the next free unit, with the type beside it that
    /// names its role.
    inline void paint(osg::StateSet& state, osg::Image& image, TextureRole role = TextureRole::Diffuse)
    {
        // Repeating, as `NifOsg` binds a texture whose file said nothing else — `osg::Texture`'s
        // own default is to clamp, which no loader in the game leaves standing.
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(&image);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);

        const unsigned int unit = static_cast<unsigned int>(state.getTextureAttributeList().size());
        state.setTextureAttributeAndModes(unit, texture, osg::StateAttribute::ON);
        state.setTextureAttribute(
            unit, new SceneUtil::TextureType(std::string(textureRoleName(role))), osg::StateAttribute::ON);
    }

    /// The same for a texture that is nothing but a name, which is all a walk reads of most of
    /// them.
    inline void paint(osg::StateSet& state, std::string_view file, TextureRole role = TextureRole::Diffuse)
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

    /// One triangle over one vertex more than a block holds: the mesh `MeshTable::checkFits`
    /// refuses, which only a content file hands over.
    inline osg::ref_ptr<osg::Geometry> makePastOneBlock()
    {
        osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array(MeshTable::sVertexBlock + 1);
        (*positions)[1] = osg::Vec3f(1.0f, 0.0f, 0.0f);
        (*positions)[2] = osg::Vec3f(0.0f, 1.0f, 0.0f);

        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        geometry->setVertexArray(positions);
        geometry->addPrimitiveSet(makeTriangles({ 0, 1, 2 }));
        return geometry;
    }

    /// A quad whose second triangle names a fifth vertex it does not have: an index `NifOsg` hands
    /// on from a file as the file wrote it, which only a content file hands over.
    inline osg::ref_ptr<osg::Geometry> makeIndexPastItsVertices()
    {
        osg::ref_ptr<osg::Geometry> geometry = makeQuad();
        geometry->setPrimitiveSet(0, makeTriangles({ 0, 1, 2, 0, 2, 4 }));
        return geometry;
    }
}

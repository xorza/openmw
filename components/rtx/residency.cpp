#include "residency.hpp"

#include <components/resource/scenemanager.hpp>

#include "texturebuilder.hpp"

namespace Rtx
{
    osg::ref_ptr<const osg::Node> SceneContent::getTemplate(VFS::Path::NormalizedView path)
    {
        // Uncompiled, because nothing here has a context to compile for. `compile` asks the
        // loader to queue the model's OpenGL objects, and this renderer initialises no OpenGL.
        return mScenes.getTemplate(path, false);
    }

    osg::ref_ptr<const osg::Image> SceneContent::getImage(VFS::Path::NormalizedView path)
    {
        return openImage(*mScenes.getImageManager(), path);
    }
}

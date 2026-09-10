#pragma once

#include <string>

#include <osg/ref_ptr>

#include <components/resource/imagemanager.hpp>
#include <components/resource/objectcache.hpp>
#include <components/vfs/pathutil.hpp>

namespace osg
{
    class Image;
}

namespace Rtx::Testing
{
    /// An image manager a test can put a decoded image into.
    ///
    /// **The cache and not the VFS**, because `getImage` reads it first: an image written in here
    /// comes back without a file, without a reader plugin, and without the warning image a miss
    /// caches — which is `GL_RGB`, a format this renderer refuses, so a slot that opened a file
    /// would be described as unreadable and logged by name.
    class HeldImages : public Resource::ImageManager
    {
    public:
        using Resource::ImageManager::ImageManager;

        void hold(VFS::Path::NormalizedView path, osg::ref_ptr<osg::Image> image)
        {
            mCache->addEntryToObjectCache(std::string(path.value()), image);
        }
    };
}

#pragma once

#include <osg/Image>
#include <osg/Node>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

namespace Resource
{
    class SceneManager;
}

namespace Rtx
{
    /// Where a cell's content is read from, by path: a model's template, and an image.
    ///
    /// **An interface, so that a ring can be handed a model by a test that has no loader.** The
    /// game answers out of `Resource::SceneManager`, whose template is the one node every clone of
    /// the model is copied from — and so the one whose drawables the frame's walk will find — and
    /// whose image cache hands one object to a template and to whoever asks for the path, which is
    /// what lets a reading made against the one be found by the other.
    class ContentSource
    {
    public:
        virtual ~ContentSource() = default;

        /// The template at `path`, or null where nothing stands for it. Safe to call from any
        /// thread, which is what the game's loader promises of its own.
        virtual osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path) = 0;

        /// The image at `path`, or null where nothing could be read there. Safe from any thread,
        /// as the template is.
        virtual osg::ref_ptr<const osg::Image> getImage(VFS::Path::NormalizedView path) = 0;
    };

    /// The game's content, out of its scene manager.
    class SceneContent final : public ContentSource
    {
    public:
        explicit SceneContent(Resource::SceneManager& scenes)
            : mScenes(scenes)
        {
        }

        osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path) override;
        osg::ref_ptr<const osg::Image> getImage(VFS::Path::NormalizedView path) override;

    private:
        Resource::SceneManager& mScenes;
    };
}

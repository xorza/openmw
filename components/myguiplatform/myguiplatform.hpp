#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUIPLATFORM_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUIPLATFORM_H

#include <filesystem>
#include <memory>
#include <string>

#include <components/vfs/pathutil.hpp>

namespace osgViewer
{
    class Viewer;
}
namespace osg
{
    class Group;
}
namespace Resource
{
    class ImageManager;
}
namespace MyGUI
{
    class LogManager;
}
namespace VFS
{
    class Manager;
}

namespace MyGUIPlatform
{

    class GuiRenderManager;
    class DataManager;
    class LogFacility;

    /// MyGUI's log, its data manager, and whichever backend draws it, which the caller brings
    /// already made.
    class Platform
    {
    public:
        Platform(std::unique_ptr<GuiRenderManager> renderManager, const VFS::Manager* vfs,
            VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logName = "MyGUI.log");

        ~Platform();

        void shutdown();

        GuiRenderManager* getRenderManagerPtr();

        DataManager* getDataManagerPtr();

    private:
        std::unique_ptr<LogFacility> mLogFacility;
        std::unique_ptr<MyGUI::LogManager> mLogManager;
        std::unique_ptr<DataManager> mDataManager;
        std::unique_ptr<GuiRenderManager> mRenderManager;
    };

}

#endif

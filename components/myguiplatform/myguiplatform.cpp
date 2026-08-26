#include "myguiplatform.hpp"

#include "guirendermanager.hpp"
#include "myguidatamanager.hpp"
#include "myguiloglistener.hpp"

namespace MyGUIPlatform
{

    Platform::Platform(std::unique_ptr<GuiRenderManager> renderManager, const VFS::Manager* vfs,
        VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logName)
        : mLogFacility(logName.empty() ? nullptr : std::make_unique<LogFacility>(logName, false))
        , mLogManager(std::make_unique<MyGUI::LogManager>())
        , mDataManager(std::make_unique<DataManager>(resourcePath, vfs))
        , mRenderManager(std::move(renderManager))
    {
        if (mLogFacility != nullptr)
            mLogManager->addLogSource(mLogFacility->getSource());

        mRenderManager->initialise();
    }

    Platform::~Platform() = default;

    void Platform::shutdown()
    {
        mRenderManager->shutdown();
    }

    DataManager* Platform::getDataManagerPtr()
    {
        return mDataManager.get();
    }

}
